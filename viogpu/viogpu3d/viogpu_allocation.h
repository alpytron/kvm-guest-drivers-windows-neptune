#pragma once
#include "handle.h"
#include "linked_list.h"

#include "viogpum.h"
#include "virgl_hw.h"

#include "viogpu_queue.h"
#include "viogpu_device.h"

class VioGpuAdapter;
class VioGpuDeviceAllocation;

class VioGpuResource final : public HandleBase<"VIOGRESO"_M, VioGpuResource>
{
  public:
  private:
};

class VioGpuAllocation;

class VioGpuAllocationSpinLockGuard
{
  friend class VioGpuAllocation;
  public:
    ~VioGpuAllocationSpinLockGuard();
  protected:
    VioGpuAllocationSpinLockGuard(VioGpuAllocation *allocation);
  private:
    VioGpuAllocation *m_Allocation;
    KIRQL m_Irql;
};

class VioGpuAllocation final : public HandleBase<"VIOGALLO"_M, VioGpuAllocation>
{
  friend class VioGpuDeviceAllocation;
  friend class VioGpuDevice;
  friend class VioGpuAllocationSpinLockGuard;
  friend class VioGpuCommander;
  friend class VioGpuCommand;
  public:
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_BLOB_OPTIONS *options, ULONGLONG size);
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_3D_OPTIONS *options, ULONGLONG size);

    ~VioGpuAllocation(void);

    inline UINT GetId(void) const
    {
        return m_Id;
    }

    void MarkBusy();
    void UnmarkBusy();

    void SetDxPhysicalAddress(size_t DxPhysicalAddress)
    {
        m_DxPhysicalAddress = DxPhysicalAddress;
    };

    size_t GetDxPhysicalAddress() const
    {
        return m_DxPhysicalAddress;
    };

#define VIOGPU_BLOB_MEM_GUEST             0x0001
#define VIOGPU_BLOB_MEM_HOST3D            0x0002
#define VIOGPU_BLOB_MEM_HOST3D_GUEST      0x0003

#define VIOGPU_BLOB_FLAG_USE_MAPPABLE     0x0001
#define VIOGPU_BLOB_FLAG_USE_SHAREABLE    0x0002
#define VIOGPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004

    inline BOOL IsCoherent() const
    {
        // FIXME: what's this even supposed to mean for blob resources?
        return (!m_IsBlob && (m_3dOptions.flags & VIRGL_RESOURCE_FLAG_MAP_COHERENT) != 0) || IsGuestBlob();
    }

    inline BOOL IsBlob() const
    {
        return m_IsBlob;
    }

    inline BOOL IsGuestBlob() const
    {
        return m_IsBlob && (m_Blob.Options.blob_mem == VIOGPU_BLOB_MEM_GUEST || m_Blob.Options.blob_mem == VIOGPU_BLOB_MEM_HOST3D_GUEST);
    }

    inline BOOL IsHost3dBlob() const
    {
        return m_IsBlob && (m_Blob.Options.blob_mem == VIOGPU_BLOB_MEM_HOST3D || m_Blob.Options.blob_mem == VIOGPU_BLOB_MEM_HOST3D_GUEST);
    }

    inline BOOL IsCreated() const
    {
        return !m_IsBlob || m_Blob.Created;
    }

    inline BOOL IsMappable() const
    {
        return m_IsBlob && m_Blob.Created && (m_Blob.Options.blob_flags & VIOGPU_BLOB_FLAG_USE_MAPPABLE) != 0;
    }

    inline BOOL IsMapped() const
    {
        return m_IsBlob && m_Blob.Created && m_Blob.Mapped;
    }

    void AttachBacking(MDL *pMdl, size_t pageCount, size_t pageOffset);
    void DetachBacking();

    void FlushToScreen(UINT scan_id);

    static NTSTATUS GetStandardAllocationDriverData(DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation);
    static NTSTATUS DxgkCreateAllocation(VioGpuAdapter *adapter, DXGKARG_CREATEALLOCATION *pCreateAllocation);

    NTSTATUS DescribeAllocation(DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation);
    NTSTATUS MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);
    NTSTATUS UnmapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);

    NTSTATUS EscapeResourceInfo(VIOGPU_RES_INFO_REQ *resInfo);
    NTSTATUS EscapeResourceBusy(VIOGPU_RES_BUSY_REQ *resBusy);
    NTSTATUS EscapeResourceBlobSetInfo(VIOGPU_RES_BLOB_SET_INFO_REQ *resBlob);

    VOID CreateBlob(UINT ctx_id);
    VOID MapBlob(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);
    VOID UnmapBlob(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);

  protected:
    BOOL m_IsBlob;
    union {
        VIOGPU_RESOURCE_3D_OPTIONS m_3dOptions;
        struct {
            VIOGPU_RESOURCE_BLOB_OPTIONS Options;
            VIOGPU_BLOB_INFO Info;
            ULONGLONG MapOffset;
            BOOL Mapped;
            BOOL InfoValid;
            BOOL Created;
        } m_Blob;
    };
    ULONGLONG m_Size;

    VioGpuAllocationSpinLockGuard LockGuard() {
        return VioGpuAllocationSpinLockGuard{this};
    }

    VOID Lock(KIRQL *OldIrql);
    VOID Unlock(KIRQL Irql);

  public:
    VioGpuDeviceAllocation *Open(VioGpuDevice *pDevice);
    void Close(VioGpuDeviceAllocation *pDeviceAllocation);
  private:
    inline LinkedList<VioGpuDeviceAllocation>::Entry *Find(VioGpuDevice *pDevice);
    inline VOID MapBlobLocked(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);
    inline VOID UnmapBlobLocked(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);

    VioGpuAdapter *m_adapter;
    UINT m_Id;

    KSPIN_LOCK m_Lock;

    LinkedList<VioGpuDeviceAllocation> m_DeviceAllocations;

    MDL *m_pMDL;
    size_t m_pageCount;
    size_t m_pageOffset;

    size_t m_DxPhysicalAddress;

    KEVENT m_busyNotification;
    volatile LONG m_busy;
};

extern void NotifyResourceDestroyed(void *ctx, void *cmd, void *resp);
