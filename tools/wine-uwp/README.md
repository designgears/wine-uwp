# wine-uwp tooling

Goal and status: `docs/LinuxPort/Plan.md`, `docs/LinuxPort/Findings-M0.md`, `docs/LinuxPort/Overrides.md`.

- `build.sh` builds `probes/*` and `shims/*` as Windows x64 PE files on Linux (clang-cl + lld-link + xwin).
- `stage.sh` assembles the calculator package with framework DLLs and shims into `~/uwp-bin/stage/calculator`.
- `run.sh [exe]` runs it in the `~/.wine-uwp` prefix with `+loaddll,+combase,+winrt` tracing.
- `probes/probe-load.cpp`: LoadLibrary a DLL and report. `probes/probe-activate.cpp`: RoGetActivationFactory a class.
- `shims/mrt100/`: stand-in for the OS `mrt100.dll` (.NET Native runtime services), see Findings-M0.

One-time setup: `cargo install xwin && xwin --accept-license splat --output ~/xwin`, `wineboot` with
`WINEPREFIX=~/.wine-uwp`, `python3 -m venv ~/uwp-bin/venv && ~/uwp-bin/venv/bin/pip install pefile`.

- `mrt-single-module.reg`: keeps private COM and WinRT MRT activation on the same prefix DLL;
  `install-overrides.sh` reapplies it on launch (see `docs/LinuxPort/Findings-M4.md`).
- `probe-mrt`: checks private framework-theme candidate resolution and fails if COM and WinRT
  use different MRT module copies. Run `tools/wine-uwp/run.sh ~/uwp-bin/build-probes/probe-mrt.exe`.

Naming a private vtable slot for slot (how the 138-slot `INavigationClient` map in
`docs/LinuxPort/Findings-M4.md` was made): `pdbsyms.py <dll>` to cache the PDB publics, find the
class's `??_7<Class>@...6B<Interface>@...@` symbol for the vtable RVA and the next `??_7` symbol
for its length, then read the pointers with `pefile` (`pe.get_data(rva, 8 * slots)`), subtract
`OPTIONAL_HEADER.ImageBase` and resolve each through the cached `.syms` with `llvm-cxxfilt`.
Parameter shapes come from disassembling the implementation (`objdump -D -b binary -m i386:x86-64
-M intel`), which is how `VIEW_BOUNDS` was sized.

- `probes/probe-dwrite.cpp` — the private DirectWrite interfaces XAML's text core needs:
  `IDWritePrivateTextAnalyzer::GetContentReadingDirection` against Windows 11's behaviour (strong
  L/R/AL character, or the paragraph direction flagged ambiguous), `IDWriteTextFormatDebug` on a
  text format (QI, locale name list and flag round trips), and `IDWriteTextLayout4` font axis values
  (set on a sub-range, counts inside/outside it, round trip, short buffer). Run
  `tools/wine-uwp/run.sh ~/uwp-bin/build-probes/probe-dwrite.exe`.
- `probes/probe-fontcollection.cpp` — 8 threads calling `GetSystemFontCollection` on one shared
  dwrite factory while another thread rewrites `HKLM\...\Fonts` to force system font set rebuilds,
  then builds a collection from the system font set. Stress test for the factory's system collection
  cache (see Findings-M4). It has not reproduced the app's crash, even on the unfixed dwrite. Run
  `tools/wine-uwp/run.sh ~/uwp-bin/build-probes/probe-fontcollection.exe`.
- `probes/probe-surface.cpp` — drives dcomp's composition path the way XAML does and checks
  each step on real pixels: CreateInteropDevice -> CreateSurfaceFactory -> CreateSurface ->
  BeginDraw (must be a real D2D device context with a target) -> draw red -> EndDraw, then attach
  the surface to an interop visual with SetContent, hand dcomp a window, and read the pixel back;
  then translate the visual with SetTransform and clip it with SetClip and check the red moves and
  is cut. It needs no activation, so it tests composition without winning the activation race:
  `tools/wine-uwp/run.sh ~/uwp-bin/build-probes/probe-surface.exe`.
