# Building the viogpu3d driver

`viogpu3d` is the WDDM **kernel-mode driver (KMD)** for the VirtIO GPU. It binds
the `PCI\VEN_1AF4&DEV_1050` device and, through its INF, registers the **Neptune**
user-mode driver (`neptune_umd.dll`, Mesa's D3D-over-virtio implementation) as the
Direct3D UMD and the Venus Vulkan ICD (`virtio_icd.json` → `vulkan_virtio.dll`) as
the Vulkan driver.

The two halves are built separately and then packaged together:

1. Build the **Mesa user-mode driver** and install it to a prefix directory.
2. Point the KMD build at that prefix (`MESA_PREFIX`); the KMD project then
   packages the UMD DLLs alongside `viogpu3d.sys` into a signed, installable
   driver.

If `MESA_PREFIX` is not set (or the UMD is not found under it), only
`viogpu3d.sys` is built and INF/package generation is skipped — you get a KMD
with no user-mode driver.

This document covers **x86**, **x64**, and **ARM64** targets. ARM64 has an extra
step (an ARM64X user-mode driver) so that x64-emulated Direct3D applications can
also use the device; see *ARM64X* below.

## Prerequisites

- **Visual Studio 2022** with the *Desktop development with C++* workload,
  including the build tools for each architecture you target (**x86**, **x64**,
  **ARM64**, and — for ARM64X — **ARM64EC**).
- **WDK / EWDK** matching your target SDK, for the KMD build.
- **Python** (with `pip`), **CMake**, **Git**, `ninja`, and a MSVC-capable
  **glslang** — for building Mesa. `meson`, `mako`, `packaging`, and `pyyaml`
  are installed via `pip`.
- A **static LLVM** built for the *target* architecture — the Mesa
  `virgl`/`llvmpipe`/`swrast` drivers link against it. When cross-compiling
  (e.g. building the arm64 UMD on an x64 host) you also need a *native*
  `llvm-tblgen`. (The Neptune-only ARM64EC view described later builds with LLVM
  disabled and does not need this.)
- The repo's test-signing certificate, `build\VirtIOTestCert.pfx`, or your own
  set up as a VS test certificate.

## 1. Build the Mesa user-mode driver

Clone the Mesa fork that carries the Neptune driver:

> **https://github.com/osy/virtio-win-mesa**

Configure it with Meson for a Windows, statically-linked build, enabling Neptune
and the Venus (virtio) Vulkan driver. Substitute a prefix directory of your
choice for `<PREFIX>`:

```
meson setup build ^
  --prefix=<PREFIX> ^
  --default-library=static -Dbuildtype=release -Db_ndebug=true -Db_vscrt=mt ^
  -Dc_args="/experimental:c11atomics" ^
  -Dplatforms=windows -Dvideo-codecs= ^
  -Dllvm=enabled ^
  -Dgallium-drivers=virgl,llvmpipe -Dvulkan-drivers=virtio,swrast ^
  -Dneptune=true -Dnpt_wine=false -Dnpt_umd=ddi
ninja -C build install
```

The options that matter:

- `-Dneptune=true` — build the Neptune Direct3D UMD, `neptune_umd.dll`.
- `-Dnpt_wine=false` — build the Windows UMD (not the Linux/Wine unixlib).
- `-Dnpt_umd=ddi` — build UMD compatible with WDDM.
- `-Dvulkan-drivers=virtio` — build the Venus Vulkan ICD (`vulkan_virtio.dll`
  and its `virtio_icd.json` manifest).
- `--default-library=static`, `-Db_vscrt=mt` — static link everything so no
  extra runtime DLLs need shipping.
- `-Dc_args="/experimental:c11atomics"` — required by Mesa under MSVC.

After `ninja install`, the prefix contains the UMD in `<PREFIX>\bin`:
`neptune_umd.dll`, `vulkan_virtio.dll`, and `virtio_icd.json`.

### Per-architecture

Build for the architecture the guest runs:

- **x86 / x64** — configure and build in a Visual Studio developer prompt for
  that architecture (e.g. `x86` or `amd64`). No cross file is needed when the
  host and target architectures match.
- **ARM64** — build natively on Windows-on-ARM, or cross-compile from x64 by
  adding a Meson `--cross-file` describing the `aarch64-pc-windows-msvc`
  toolchain and pointing at an arm64 LLVM. (See the Mesa repo for cross-file
  examples.)

## 2. Build the kernel-mode driver

Set `MESA_PREFIX` to the prefix from step 1 (the directory whose `bin`
subdirectory holds `vulkan_virtio.dll`):

```
set MESA_PREFIX=<PREFIX>
```

When `%MESA_PREFIX%\bin\vulkan_virtio.dll` exists, the `viogpu3d` project
regenerates the Vulkan ICD manifest for that prefix
(`viogpu3d\tools\update_icd_jsons.ps1`) and packages `neptune_umd.dll`,
`vulkan_virtio.dll`, and `virtio_icd.json` into the driver, producing a complete,
installable package. If it is unset, only `viogpu3d.sys` is built.

Then build the driver from the `viogpu` directory. The simplest route is the
convenience wrapper (it builds the whole `viogpu.sln` and skips the slow Static
Driver Verifier pass):

```
cd viogpu
build_AllNoSdv.bat
```

By default this builds for **ARM64** and then for **x86 + x64**, targeting both
Win10 and Win11. To build a single configuration, call the master build script
directly (`<arch>` = `x86`, `x64`, or `ARM64`; add `Debug` for a checked build):

```
..\build\build.bat viogpu.sln "Win10 Win11" <arch> [Debug]
```

The signed, installable package is written to
`viogpu\Install\Win10\<arch>\` (or `Install_Debug\...` for a `Debug` build). It
contains `viogpu3d.sys`, `viogpu3d.inf`, `viogpu3d.cat`, and — when `MESA_PREFIX`
was set — `neptune_umd.dll`, `vulkan_virtio.dll`, and `virtio_icd.json`. An ARM64
build additionally packages the two ARM64X view DLLs
(`neptune_umd_arm64.dll`, `neptune_umd_ec.dll`) when `MESA_ARM64X_PREFIX` is set;
see *ARM64X* below.

The package is test-signed during the build. If you sign manually, sign the
`.sys` and `.cat` with `build\VirtIOTestCert.pfx` (`signtool sign /fd SHA256`).

## ARM64X (ARM64 with x64-emulation support)

On Windows-on-ARM, a native-arm64 `neptune_umd.dll` can be loaded by native
arm64 apps but **not** by x64-emulated Direct3D apps — those need an **ARM64EC**
view of the UMD. To support both from one driver, the user-mode driver must be an
**ARM64X** image, which carries both a native-arm64 and an arm64ec view.

Windows cannot load a single DLL that satisfies both native-arm64 and arm64ec
callers, so the UMD is built twice and stitched together behind a code-less
ARM64X **forwarder**: the deliverable is **three files that ship side by side** —
the forwarder `neptune_umd.dll` (what the OS loads) plus its two view payloads
`neptune_umd_arm64.dll` (arm64) and `neptune_umd_ec.dll` (arm64ec).

The forwarder is produced by **`make-arm64x.bat`**, which lives in the Mesa fork
alongside the Neptune UMD at
`src/virtio/neptune/triton/tools/make-arm64x.bat`. It derives the forwarder's
exports from `neptune_umd.def` (the same file meson feeds the real builds, so the
forwarder can never drift from the driver's ABI), routes each input DLL to a view
by its machine type (not argument order), and writes the three-file set.

Produce and package the ARM64X UMD in three steps, after the normal arm64 UMD
build (step 1):

1. **Build a second, ARM64EC view of the UMD.** Reconfigure Mesa for arm64ec:
   add `/arm64EC` to the C and C++ arguments, use the dynamic CRT
   (`-Db_vscrt=md`), and — since the emulated view only needs Neptune — disable
   LLVM and the other drivers:

   ```
   meson setup build-ec ^
     --prefix=<PREFIX-EC> ^
     --default-library=static -Dbuildtype=release -Db_ndebug=true -Db_vscrt=md ^
     -Dc_args="/experimental:c11atomics /arm64EC" -Dcpp_args="/arm64EC" ^
     -Dllvm=disabled -Dplatforms=windows -Dvideo-codecs= ^
     -Dgallium-drivers= -Dvulkan-drivers= ^
     -Dopengl=false -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dglx=disabled ^
     -Dneptune=true -Dnpt_wine=false -Dnpt_umd=ddi ^
     --cross-file <optional arm64ec cross file>
   ninja -C build-ec
   ```

   This yields an arm64ec `neptune_umd.dll`. Building arm64ec requires the
   ARM64EC MSVC and SDK import libraries on `LIB` (the `arm64ec` variants of the
   VC and Windows SDK `um`/`ucrt` libraries).

2. **Assemble the ARM64X three-file set with `make-arm64x.bat`.** Pass it the two
   `neptune_umd.dll` builds (the native-arm64 one from step 1 of *Build the Mesa
   user-mode driver* and the arm64ec one from step 1 above), in either order, and
   an output directory:

   ```
   src\virtio\neptune\triton\tools\make-arm64x.bat ^
     <PREFIX>\bin\neptune_umd.dll <build-ec>\...\neptune_umd.dll ^
     <ARM64X-DIR>
   ```

   It classifies each input by machine type (`AA64` → native view, `8664` →
   EC view), builds the forwarder, and writes `neptune_umd.dll`,
   `neptune_umd_arm64.dll`, and `neptune_umd_ec.dll` into `<ARM64X-DIR>`. It must
   run from a shell that can reach the arm64 MSVC tools; it locates Visual Studio
   via `vswhere` and enters the arm64 developer environment itself.

3. **Package all three — point `MESA_ARM64X_PREFIX` at `<ARM64X-DIR>`.** The KMD
   build picks the three files up automatically for the **ARM64** target: it
   packages the forwarder as `neptune_umd.dll`, adds `neptune_umd_arm64.dll` and
   `neptune_umd_ec.dll` to the package, the catalog, and the INF's file/copy
   lists, and keeps `neptune_umd.dll` as `UserModeDriverName`. No manual INF or
   catalog edits are needed.

   ```
   set MESA_PREFIX=<PREFIX>
   set MESA_ARM64X_PREFIX=<ARM64X-DIR>
   ..\build\build.bat viogpu.sln Win11 ARM64
   ```

   `MESA_ARM64X_PREFIX` only affects ARM64 builds; x86 and x64 always package the
   single native `neptune_umd.dll` from `MESA_PREFIX`. If it is unset (or
   `%MESA_ARM64X_PREFIX%\neptune_umd_ec.dll` is missing), the ARM64 build falls
   back to the native-arm64 `neptune_umd.dll` and only native-arm64 apps can load
   the UMD.

The result is a single driver whose UMD loads for both native-arm64 and
x64-emulated Direct3D applications.

## Installing

On the guest, trust the test certificate in **both** the machine `Root` and
`TrustedPublisher` stores, then install the whole package:

```
pnputil /add-driver viogpu3d.inf /install
```

The KMD loads without a reboot. A healthy install shows the *Red Hat VirtIO GPU
3D controller* device *Started*, with Direct3D device creation succeeding on the
`0x1af4` adapter.

## Troubleshooting

- **Only `viogpu3d.sys` was built (no INF/package).** `MESA_PREFIX` was unset or
  `%MESA_PREFIX%\bin\vulkan_virtio.dll` was missing — build and install the Mesa
  UMD (step 1) first, then set `MESA_PREFIX`.
- **x64-emulated app fails to load the UMD on ARM64** (e.g. `ERROR_BAD_EXE_FORMAT`)
  — the UMD is native-arm64 only; build the ARM64X image as above.
- **`pnputil` reports the certificate chain is not trusted** — the test
  certificate must be present in both the `Root` and `TrustedPublisher` machine
  stores.
- **Wrong UMD architecture** — the UMD architecture must match the KMD's target:
  `x86` UMD with an `x86` KMD, `x64` with `x64`, and on ARM64 the ARM64X (or
  native-arm64) UMD with the `ARM64` KMD.
