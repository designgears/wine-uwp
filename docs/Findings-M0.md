# M0 findings (2026-09-09)

## Workshop that now exists
- `tools/wine-uwp/clang-cl-x64.cmake`: builds Windows x64 PE binaries on Linux with clang-cl + lld-link
  against SDK headers/libs fetched by `xwin` into `~/xwin`. No Windows toolchain needed.
- Probes: `probe-load` (LoadLibrary + report), `probe-activate` (RoGetActivationFactory + RoActivateInstance).
- Prefix `~/.wine-uwp` (wine 11.17, win64). Staged app at `~/uwp-bin/stage/calculator`.
- Test package: `microsoft/calculator` CI run 32911385597, artifact `Build-x64`, file
  `Calculator/Calculator_0.365.1.0_x64.msix` (Release, .NET Native, ilc 2.2.31116). Layout: 20 KB
  `CalculatorApp.exe` stub importing one export `RHBinder__ShimExeMain` from 8.4 MB `CalculatorApp.dll`.
  Same shape as Shapr3D.

## Framework packages, all obtained without a Windows machine
| Package | Source | Note |
|---|---|---|
| Microsoft.UI.Xaml.2.8 (8.2501.31001.0) | NuGet `microsoft.ui.xaml/2.8.7`, `tools/AppX/x64/Release/*.appx` | `Microsoft.UI.Xaml.dll` **loads in Wine as is** |
| Microsoft.NET.Native.Runtime.2.2 (2.2.27328.0) | NuGet `runtime.win10-x64.microsoft.net.native.compiler/2.2.3`, `tools/Runtime/x64/*.appx` | `mrt100_app.dll`, `mrt_map.dll` |
| Microsoft.NET.Native.Framework.2.2 (2.2.27405.0) | NuGet `runtime.win10-x64.microsoft.net.native.sharedlibrary/2.2.3`, `tools/SharedLibrary/ret/Native/*.appx` | `SharedLibrary.dll` 13 MB |
| Microsoft.VCLibs.140.00.Desktop | `https://aka.ms/Microsoft.VCLibs.x64.14.00.Desktop.appx` | UWP `*_APP.dll` names satisfied by copying the desktop DLLs under `_app` names |

Both .NET Native packages are older than the manifest MinVersion (28604 / 29512). Loads fine; watch for
missing SharedLibrary exports at runtime.

## Static import map of the package
- `CalculatorApp.dll` -> `mrt100_app.dll`, `SharedLibrary.dll`, `clrcompression.dll`, api-sets, bcrypt/ncrypt/crypt32.
- `CalcManager.dll`, `TraceLogging.dll`, `GraphControl.dll` -> VC `_APP` runtimes, api-sets, d2d1/d3d11/dwrite/dxgi (GraphControl).
- Everything XAML/WinRT is reached dynamically through `RoGetActivationFactory`. So load-time gaps were
  only the framework packages; the runtime gaps start at activation.

## Load results under Wine 11.17
| Binary | Result |
|---|---|
| CalcManager.dll, TraceLogging.dll, Microsoft.Web.WebView2.Core.dll | OK |
| Microsoft.UI.Xaml.dll (WinUI 2) | OK |
| GraphControl.dll | FAIL in DllMain: `err:ole:ensure_mta Failed, hr 0x800401f0` then C++ exception. Not needed for first run. |
| mrt100_app.dll, SharedLibrary.dll, CalculatorApp.dll | FAIL until the `mrt100.dll` stand-in existed, then OK |

## Wine gap #1: delay-load failure semantics (fixed with a stand-in, needs a Wine patch)
`mrt100_app.dll` delay-loads `mrt100.dll!GetManagedRuntimeService(REFGUID, IUnknown**)` from DllMain,
wrapped in `__try`. `mrt100.dll` is an OS DLL Wine does not have. On Windows, the failed delay-load raises
exception code `ERROR_DELAY_LOAD_FAILED` (0x505) with `ExceptionInformation[0] = DelayLoadInfo*`, and the
app's filter (`RVA 0x831e4`: `code == 0x505`) takes a built-in fallback object (`RVA 0x2e1f0`).
Wine's `kernelbase!DelayLoadFailureHook` raises `EXCEPTION_WINE_STUB` instead, which nothing catches, and
the process dies in DllMain with an access violation.

Stand-in: `tools/wine-uwp/shims/mrt100/mrt100.cpp` exports the function and raises 0x505 itself.
Proper fix: patch Wine so delay-load failure raises 0x505 with a `DelayLoadInfo` (Windows 8+ behaviour),
then the stand-in is unnecessary. Candidate first upstream patch.

## Next activation gaps seen
- `Windows.System.MemoryManager`: no library registered in Wine. Requested during runtime init, non-fatal so far.

## First launch of the real packaged app (`wine CalculatorApp.exe`)
Sequence observed with `WINEDEBUG=+loaddll,+combase`:
1. Stub exe loads `CalculatorApp.dll` -> `mrt100_app.dll` -> `SharedLibrary.dll` -> `clrcompression.dll`. All OK.
2. `mrt100.dll` stand-in takes the fallback path. Runtime init continues.
3. `RoGetActivationFactory("Windows.System.Threading.ThreadPool")` -> served by Wine's `threadpoolwinrt.dll`. First
   real Windows-Runtime call from a .NET Native app answered by Wine.
4. `RoGetActivationFactory("Windows.System.MemoryManager")` -> not in Wine (non-fatal here).
5. `RoGetActivationFactory("Windows.UI.Xaml.Application", IID {06499997-...})` -> **no library**. This is `Application.Start`.
6. `RoGetActivationFactory("Windows.ApplicationModel.Resources.Core.ResourceContext")` x7 -> no library (`MrmCoreR.dll`).
7. Fail-fast `0xc0000602` from `SharedLibrary.dll`.

So M0 ends exactly at the predicted boundary: everything up to the OS XAML engine and the resource manager
works from redistributable bits plus one stand-in. From here the Windows binaries in `Overrides.md` are required.
