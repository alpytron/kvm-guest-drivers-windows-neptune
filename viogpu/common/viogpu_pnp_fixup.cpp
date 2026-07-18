/*
 * Copyright (C) 2026 Turing Software, LLC
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "helper.h"
#include "viogpu_pnp_fixup.h"
#if !DBG
#include "viogpu_pnp_fixup.tmh"
#endif

// The original IRP_MJ_PNP dispatch of the PDO's DriverObject, saved so we can
// forward everything we don't handle and restore it on removal. A single
// pointer is enough: virtio-gpu enumerates a single adapter and the hook is
// per-DriverObject, not per-device.
static PDRIVER_DISPATCH gOriginalPnpIrp;

//
// SystemBootGraphicsInformation lets us recover the boot framebuffer range
// that win32k expects to find in the PDO's resource list. These definitions
// are not exposed by the WDK headers.
//
typedef enum _VIOGPU_SYSTEM_INFORMATION_CLASS
{
    VioGpuSystemBootGraphicsInformation = 0x7e
} VIOGPU_SYSTEM_INFORMATION_CLASS;

typedef enum _VIOGPU_SYSTEM_PIXEL_FORMAT
{
    VioGpuSystemPixelFormatUnknown,
    VioGpuSystemPixelFormatR8G8B8,
    VioGpuSystemPixelFormatR8G8B8X8,
    VioGpuSystemPixelFormatB8G8R8,
    VioGpuSystemPixelFormatB8G8R8X8
} VIOGPU_SYSTEM_PIXEL_FORMAT;

typedef struct _VIOGPU_SYSTEM_BOOT_GRAPHICS_INFORMATION
{
    LARGE_INTEGER FrameBuffer;
    ULONG Width;
    ULONG Height;
    ULONG PixelStride;
    ULONG Flags;
    VIOGPU_SYSTEM_PIXEL_FORMAT Format;
    ULONG DisplayRotation;
} VIOGPU_SYSTEM_BOOT_GRAPHICS_INFORMATION, *PVIOGPU_SYSTEM_BOOT_GRAPHICS_INFORMATION;

extern "C" NTSTATUS WINAPI ZwQuerySystemInformation(
    _In_ VIOGPU_SYSTEM_INFORMATION_CLASS SystemInformationClass,
    _Inout_ PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength);

// OR'd into the minor function of the IRP we send down so our own dispatch can
// recognize the reentry and forward it straight to the original handler
// instead of recursing.
#define IRP_MN_CUSTOM_INJECTED 0x80

static NTSTATUS GetFramebufferAddress(_Out_ ULONGLONG *pStartAddress, _Out_ ULONGLONG *pEndAddress)
{
    NTSTATUS Status;
    VIOGPU_SYSTEM_BOOT_GRAPHICS_INFORMATION SystemBootGraphicsInfo;
    ULONG PixelBytes;

    Status = ZwQuerySystemInformation(VioGpuSystemBootGraphicsInformation,
                                      &SystemBootGraphicsInfo,
                                      sizeof(SystemBootGraphicsInfo),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    if (SystemBootGraphicsInfo.Format == VioGpuSystemPixelFormatB8G8R8)
    {
        PixelBytes = 3;
    }
    else if (SystemBootGraphicsInfo.Format == VioGpuSystemPixelFormatB8G8R8X8)
    {
        PixelBytes = 4;
    }
    else
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    *pStartAddress = SystemBootGraphicsInfo.FrameBuffer.QuadPart;
    *pEndAddress = *pStartAddress + (ULONGLONG)SystemBootGraphicsInfo.Height * SystemBootGraphicsInfo.PixelStride * PixelBytes;

    return STATUS_SUCCESS;
}

static NTSTATUS InjectFramebufferResource(_Inout_ PCM_RESOURCE_LIST *ppResourceList)
{
    NTSTATUS status;
    PCM_RESOURCE_LIST pResourceList;
    SIZE_T resourceListSize;
    PCM_FULL_RESOURCE_DESCRIPTOR list;
    ULONGLONG framebufferStart, framebufferEnd;
    BOOLEAN foundFramebuffer;

    status = GetFramebufferAddress(&framebufferStart, &framebufferEnd);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    pResourceList = *ppResourceList;
    list = pResourceList->List;
    foundFramebuffer = FALSE;

    for (ULONG ix = 0; ix < pResourceList->Count; ++ix)
    {
        /* Process resources in CM_FULL_RESOURCE_DESCRIPTOR block number ix. */

        for (ULONG jx = 0; jx < list->PartialResourceList.Count && !foundFramebuffer; ++jx)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;
            ULONGLONG memoryStart, memoryLength, memoryEnd;

            desc = list->PartialResourceList.PartialDescriptors + jx;

            if (desc->Type != CmResourceTypeMemory && desc->Type != CmResourceTypeMemoryLarge)
            {
                continue;
            }
            memoryLength = RtlCmDecodeMemIoResource(desc, &memoryStart);
            memoryEnd = memoryStart + memoryLength;

            if (framebufferStart >= memoryStart && framebufferEnd <= memoryEnd)
            {
                foundFramebuffer = TRUE;
                break;
            }
        }

        /* Advance to next CM_FULL_RESOURCE_DESCRIPTOR block in memory. */

        list = (PCM_FULL_RESOURCE_DESCRIPTOR)(list->PartialResourceList.PartialDescriptors +
                                              list->PartialResourceList.Count);
    }

    if (!foundFramebuffer)
    {
        CM_FULL_RESOURCE_DESCRIPTOR newRes;
        ULONGLONG framebufferLength;

        /* We need to re-allocate with room for a new resource descriptor */
        resourceListSize = (UINT_PTR)list - (UINT_PTR)pResourceList;
        pResourceList = (PCM_RESOURCE_LIST)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                                       resourceListSize + sizeof(CM_FULL_RESOURCE_DESCRIPTOR),
                                                                       VIOGPUTAG);
        if (!pResourceList)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(pResourceList, *ppResourceList, resourceListSize);
        ExFreePoolWithTag(*ppResourceList, 0);
        RtlZeroMemory(&newRes, sizeof(newRes));
        newRes.PartialResourceList.Version = 1;
        newRes.PartialResourceList.Revision = 1;
        newRes.PartialResourceList.Count = 1;
        framebufferLength = framebufferEnd - framebufferStart;
        if (RtlCmEncodeMemIoResource(&newRes.PartialResourceList.PartialDescriptors[0],
                                     CmResourceTypeMemory,
                                     framebufferLength,
                                     framebufferStart) == STATUS_UNSUCCESSFUL)
        {
            RtlCmEncodeMemIoResource(&newRes.PartialResourceList.PartialDescriptors[0],
                                     CmResourceTypeMemoryLarge,
                                     framebufferLength,
                                     framebufferStart);
        }
        RtlCopyMemory((char *)pResourceList + resourceListSize, &newRes, sizeof(newRes));
        pResourceList->Count++;
        *ppResourceList = pResourceList;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS VioGpuDisplayFixupPnpIrp(IN PDEVICE_OBJECT pDevObj, IN PIRP pIrp)
{
    PIO_STACK_LOCATION pStack;
    KEVENT event;
    NTSTATUS status;
    IO_STATUS_BLOCK ioStatus;
    PIRP newIrp;

    pStack = IoGetCurrentIrpStackLocation(pIrp);
    if (pStack->MajorFunction != IRP_MJ_PNP || pStack->MinorFunction != IRP_MN_QUERY_RESOURCES)
    {
        if (pStack->MajorFunction == IRP_MJ_PNP)
        {
            // This is the IRP we injected below reentering our dispatch; unset
            // the custom flag and let the original handler service it.
            pStack->MinorFunction &= ~IRP_MN_CUSTOM_INJECTED;
        }
        return gOriginalPnpIrp(pDevObj, pIrp);
    }

    // Only modify IRP_MN_QUERY_RESOURCES
    KeInitializeEvent(&event, SynchronizationEvent, FALSE);

    newIrp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, pDevObj, NULL, 0, 0, &event, &ioStatus);
    if (!newIrp)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    newIrp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoGetNextIrpStackLocation(newIrp)->MinorFunction = IRP_MN_QUERY_RESOURCES | IRP_MN_CUSTOM_INJECTED;

    status = IoCallDriver(pDevObj, newIrp);

    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event,
                              Executive,  // WaitReason
                              KernelMode, // must be Kernelmode to prevent the stack getting paged out
                              FALSE,
                              NULL // indefinite wait
        );
        status = ioStatus.Status;
    }

    status = InjectFramebufferResource((PCM_RESOURCE_LIST *)&ioStatus.Information);

    pIrp->IoStatus.Information = ioStatus.Information;
    pIrp->IoStatus.Status = status;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return status;
}

void VioGpuInstallDisplayFixup(_In_ PDEVICE_OBJECT pPhysicalDeviceObject)
{
    PDRIVER_OBJECT pDriverObject = pPhysicalDeviceObject->DriverObject;

    if (gOriginalPnpIrp == NULL)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("Patching IRP_MJ_PNP to workaround double display bug\n"));
        gOriginalPnpIrp = pDriverObject->MajorFunction[IRP_MJ_PNP];
        pDriverObject->MajorFunction[IRP_MJ_PNP] = VioGpuDisplayFixupPnpIrp;
    }
}

void VioGpuRemoveDisplayFixup(_In_ PDEVICE_OBJECT pPhysicalDeviceObject)
{
    if (gOriginalPnpIrp)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("Removing IRP_MJ_PNP patch\n"));
        pPhysicalDeviceObject->DriverObject->MajorFunction[IRP_MJ_PNP] = gOriginalPnpIrp;
        gOriginalPnpIrp = NULL;
    }
}
