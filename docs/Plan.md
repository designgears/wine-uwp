# wine-uwp: running packaged UWP apps under Wine

Goal (revised 2026-09-09): a Wine module set plus launcher that runs **packaged UWP apps we do not have source
for** (target: Shapr3D). Windows Calculator is test subject #1 because it is small, its source lets us build
minimal probes, and its GitHub CI publishes real `.msixbundle` artifacts compiled with .NET Native, the same
shape as Shapr3D. Test subject #2 is Shapr3D itself.

Rule: one probe at a time, each answers one question, each ends in a written finding. Grab existing binaries
and projects wherever they exist; build only the layer nobody has.

## 1. What a UWP process needs that Wine does not have

A packaged UWP app is not a Win32 program. `Shapr3D.exe` / `CalculatorApp.exe` is a stub; the app lives in a
DLL activated through WinRT. From the bottom up, the stack is:

| Layer | Windows binaries | Wine today | Plan |
|---|---|---|---|
| WinRT activation (`RoGetActivationFactory`, `ActivatableClassId` registry) | `combase.dll` | Implemented | Use as is. Register classes from the manifest. |
| Package identity (`GetCurrentPackageFullName`, `GetPackagePath`, `PackageFamilyName`) | `kernelbase.dll`, StateRepository DB | Stubs returning "no package" | **Build**: registry-backed implementation in Wine's kernelbase. |
| Resources (`resources.pri`, `ResourceLoader`, `ms-appx://`) | `MrmCoreR.dll`, `Windows.UI.Xaml.Resources.*.dll` | None | Grab `MrmCoreR.dll` as native override first; write a PRI reader only if it fails. |
| Core app model (`CoreApplication`, `ApplicationView`, activation args) | `twinapi.appcore.dll`, `Windows.ApplicationModel.dll` | Thin stubs | Grab real DLL as override. Launcher fakes the shell's activation call. |
| Core window and dispatcher | `Windows.UI.dll` (`CoreWindow`, `Windows.UI.Composition`), `CoreMessaging.dll`, `InputHost.dll` | Stubs | Grab as overrides; expect win32k-level gaps to patch in Wine. |
| Composition | `dcomp.dll` -> `NtDComposition*` syscalls -> `dwm.exe` | Stub `dcomp.dll`, no DWM | **The core of the project.** See section 3. |
| XAML engine | `Windows.UI.Xaml.dll` (~20 MB) | 62 KB stub | Grab as override. Never reimplement. |
| WinUI 2 | `Microsoft.UI.Xaml.dll` in framework package `Microsoft.UI.Xaml.2.8` | n/a | Redistributable, ships in the NuGet package. |
| .NET Native runtime | framework packages `Microsoft.NET.Native.Runtime.2.2`, `Microsoft.NET.Native.Framework.2.2` (`mrt100_app.dll`, `SharedLibrary.dll`) | n/a | Grab (Store packages, plain zips). Plain native code, no CLR to host. |
| VC runtime | `Microsoft.VCLibs.140.00` | `vcrun2022` via winetricks covers the exports | Use winetricks, or grab the appx. |
| Brokered APIs | `RuntimeBroker.exe` (out-of-proc COM for file pickers, clipboard, etc.) | None | Ignore until an app blocks on it; most calls fail soft. |
| AppContainer sandbox | token/capabilities | None | Skip entirely. Run full trust. |

Everything marked "grab" is copied from a Windows 11 install or Store package the user is licensed for. That is
fine for personal use and for research. A distributable product later would need clean-room replacements for
those pieces, so we keep a list (`docs/LinuxPort/Overrides.md`) of exactly which Microsoft binaries are loaded
and which exports they pull, so the boundary is always known.

## 2. Test subjects and probes we can build on Linux

- Real packages, no Windows box needed:
  - Calculator: `gh run download` from `microsoft/calculator` CI (Release, x64, .NET Native). Unsigned test
    build, and we do not verify signatures anyway.
  - Shapr3D: `Shapr3D.Package_*_x64.msixbundle` (already unpacked once; ~1.5 GB, 580 GB free here).
  - Framework packages: `Microsoft.UI.Xaml.2.8.appx` from NuGet, `Microsoft.VCLibs.140.00` from the
    Windows App SDK repo, `.NET.Native.*` from the Store CDN.
- Probes we compile ourselves with clang-cl + lld-link against Windows SDK headers pulled by `xwin`
  (already have clang-cl and lld-link; no Windows toolchain needed):
  1. `probe-load`: `LoadLibrary` a DLL and print unresolved imports. Day 1 map of Wine gaps.
  2. `probe-activate`: `RoGetActivationFactory("Windows.Globalization.Calendar")` against the real DLL.
  3. `probe-corewindow`: `CoreApplication::CreateNewView`, show a `CoreWindow`.
  4. `probe-island`: Win32 exe + XAML Islands (`DesktopWindowXamlSource`) hosting one `Button`. First XAML pixels.
  5. `probe-uwp`: minimal C++/WinRT UWP app (`Application::Start`), packaged by our own launcher.
- The calculator source stays useful for probe 5 and for building variant apps that isolate one API at a time.

## 3. Composition: the piece we have to build

`Windows.UI.Xaml.dll` draws through `Windows.UI.Composition` (in `Windows.UI.dll`), which talks to `dcomp.dll`
through private interfaces, and `dcomp.dll` submits batches to the desktop window manager through
`NtDCompositionCreateChannel` / `NtDCompositionProcessChannelBatchBuffer` in `win32u`. No DWM exists in Wine.

Two ways to cut it, decided by the `probe-island` finding:

- **Cut A (preferred): implement `dcomp.dll` in Wine.** Public `IDCompositionDevice*` interfaces are documented.
  The private ones `Windows.UI.dll` uses are not, so we recover them from the real `dcomp.dll` export table
  and vtables (IDA/Ghidra on our own copies) and implement a retained visual tree rendered with D3D11 via
  wined3d/vkd3d onto the `CoreWindow` HWND. Wine is LGPL; this lands upstream as `dlls/dcomp`.
- **Cut B: keep real `dcomp.dll`, implement the syscalls plus a compositor process.** The batch buffer format is
  undocumented and versioned; more reverse engineering, less API surface. Fallback only if Cut A's private
  interface count is unreasonable.

Either way this is months, not days. The rest of the plan is arranged so that everything else is proven before
we commit to it, and so that progress (windows, activation, resources, launcher) is visible before pixels.

## 4. Milestones

### M0: Workshop (2 days)
- [x] `xwin` to fetch Windows SDK headers/libs; `cmake` toolchain file for clang-cl targeting x64 Windows.
- [x] Prefix `~/.wine-uwp` on wine-cachyos (also test plain wine 11.17). `winetricks vcrun2022`. Script it.
- [x] `tools/wine-uwp/` in this repo: `unpack.py` (msixbundle -> msix -> dir, parse `AppxManifest.xml`),
  `overrides.sh` (copy listed DLLs, set `WINEDLLOVERRIDES` native-first), `probe-load` built and running.
- [x] Gather binaries into `~/uwp-bin/` with a manifest of source and SHA256.
- Done when: `probe-load Windows.UI.Xaml.dll` prints the unresolved import list. That list *is* the roadmap
  for M1.

### M1: WinRT activation of real system DLLs (1 week) — DONE 2026-09-09, see Findings-M1.md
- [x] Registry writer: `gen-classes-reg.py` from the SOFTWARE hive, 4040 classes.
- [x] Real `Windows.UI.Xaml.dll`, `twinapi.appcore.dll`, `MrmCoreR.dll`, `rmclient.dll` activate and run.
- [x] Package identity: env-driven (`WINE_UWP_*`) in kernelbase + ntdll, incl. token security attributes and package graph.
- [x] Private combase ordinals pinned and implemented (`dlls/combase/asta.c`); activation registrar (`dlls/wineuwp`).
- Done when: a real Microsoft WinRT DLL activates and answers a call inside Wine.

### M2: CoreWindow (1 to 2 weeks) — blocked on ALPC first
- [ ] wineserver ALPC: connect/accept/send-wait-receive/disconnect with message attributes. CoreMessaging cannot start without its registrar port.
- [ ] Host the CoreMessagingRegistrar (real `CoreMessaging.dll` `ServiceMain` in Wine's svchost, or `CoreUIInitializeTestService` in-process).
- [ ] Overrides: `twinapi.appcore`, `Windows.UI`, `CoreMessaging`, `InputHost`. Fix each unresolved import
  in Wine (`win32u`, `user32`, `ntdll` gaps). Track them in `Overrides.md`.
- [ ] `probe-corewindow` shows an empty `CoreWindow` HWND and receives pointer/keyboard events.
- Done when: an HWND of class `Windows.UI.Core.CoreWindow` appears on the X11/Wayland desktop.

### M3: First XAML pixels via XAML Islands (2 to 4 weeks, opens the composition decision)
- [ ] Override `Windows.UI.Xaml.dll` + `Windows.UI.Xaml.Resources.*` + `MrmCoreR.dll`.
- [ ] `probe-island` reaches the first `dcomp` call. Record every private `dcomp` interface requested.
- [ ] Spike: minimal `dcomp.dll` that satisfies device creation and draws visuals with solid colors to the
  HWND with D3D11. A `Button` rendered at all is the milestone, no matter how ugly.
- Decision gate: Cut A vs Cut B, with counted interface/method surface.

### M4: Composition for real (months)
- [ ] Implement `dcomp.dll`: visuals, surfaces, transforms, clips, effects, animations (XAML uses only a subset;
  we implement in the order `probe-island` and the calculator request it).
- [ ] DirectWrite/D2D through Wine's existing implementations; fix gaps as XAML hits them.
- Done when: `probe-island` renders a `NavigationView` from `Microsoft.UI.Xaml.dll` correctly.

### M5: Launcher for packaged apps (2 to 3 weeks, can overlap M4)
- [ ] `wine-uwp run <dir-or-msixbundle>`: unpack, resolve `PackageDependency` framework packages, write
  registry (`ActivatableClassId` for every `InProcessServer` in the manifest, package identity, PRI paths),
  place framework DLLs on the load path, start `Executable`, then deliver launch activation the way the shell
  does (`IApplicationActivationManager` equivalent: CoCreate the app's `EntryPoint` class through
  `Windows.ApplicationModel.Core` and call `IFrameworkViewSource` / `Application.Start`). This part is
  reverse engineered from `twinapi.appcore.dll`.
- [ ] `probe-uwp` (our tiny UWP app) launches this way.
- Done when: `wine-uwp run Calculator_*.msixbundle` gets to `OnLaunched` (visible in `+winrt` traces) even
  if it then dies in composition.

### M6: Calculator runs (integration)
- [ ] .NET Native runtime packages load; `CalculatorApp.exe` -> `CalculatorApp.dll` -> `OnLaunched` -> MainPage.
- [ ] Standard mode adds two numbers. Screenshot goes in `docs/LinuxPort/`.

### M7: Shapr3D (integration, new walls)
- Extra pieces it needs on top of the calculator: Win2D (`Microsoft.Graphics.Canvas.dll`, D2D/D3D11, Wine has
  most), WebView2 (Edge runtime under Wine, a known separate effort; may be stubbed to return "unavailable"),
  ~200 in-package activatable classes in `Shapr.Native.dll`, the SpaceMouse full-trust helper (skip),
  `Windows.Storage` pickers via `RuntimeBroker` (likely first real broker need).
- Done when: the model viewport draws.

## 5. Dropped from the previous plan
- Uno Platform port of the calculator: only helps when source exists. Off-goal now.
- Bottles/protontricks/flatpak: prefix management only, no bearing on the runtime. Flatpak returns at the very
  end for shipping.
- Proton vs Wine: same WinRT coverage. We develop against upstream Wine so patches can land there, and use
  wine-cachyos as a convenience runtime.

## 6. Risks stated plainly
- Composition (M4) is the whole game. If Cut A's private interface surface is too large and Cut B's batch
  format is opaque, the fallback is a middle path: real `dcomp.dll` plus our own `dwmcore`-shaped in-process
  compositor faked behind the syscalls. Budget the decision, not the outcome.
- Windows builds change private interfaces. We pin one Windows 11 build for every grabbed binary and record
  it in `Overrides.md`.
- Every grabbed binary is a redistribution problem later. Keep the list short and the boundary explicit.

## 7. First commands
```
cargo install xwin && xwin --accept-license splat --output ~/xwin   # Windows SDK headers/libs on Linux
sudo pacman -S mingw-w64-gcc                                        # for quick C probes
WINEPREFIX=~/.wine-uwp wineboot && WINEPREFIX=~/.wine-uwp winetricks -q vcrun2022
gh run download -R microsoft/calculator --name <artifact> -D ~/uwp-bin/calculator   # real .NET Native package
```
