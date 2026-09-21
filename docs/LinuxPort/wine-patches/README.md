# Our Wine changes

**These are not committed in the Wine tree, and until now they were not committed anywhere.**

`~/src/wine` is shared with another session and is deliberately left uncommitted, and AGENTS.md
says to commit only files in *this* repo. Read literally that left ~2,700 lines of this project's
own work living in exactly one working tree - one `git checkout` or one careless `git stash` from
gone, and invisible to anyone reading this repo or picking the work up later. The patches here fix
that without breaking the rule: nothing is committed over there, the diffs are committed *here*.

Regenerate after any change to the Wine side:

```
tools/wine-uwp/export-wine-patches.sh
```

Apply to a matching tree:

```
cd ~/src/wine && git apply docs/LinuxPort/wine-patches/*.patch
```

`BASE.txt` records the Wine commit the patches were taken against. They are plain `git diff`
output, so they will not apply cleanly to a tree that has moved on - re-export rather than
hand-merge, and check `git -C ~/src/wine status` first.

## What is in them

| Patch | What it does |
| --- | --- |
| `dlls-wineuwp-main.c.patch` | The in-process stand-ins for the shell brokers a packaged app talks to. This session added `open_app_thread()` (never queue an APC onto a Wine worker), `WINE_UWP_DUMP_STACKS`, the `SplashScreen.Dismissed` firing, and the import-thunk repair that supplies `GetPackageFamilyNameFromToken` when Wine's apiset table cannot |
| `dlls-dcomp-composition.c.patch` | Type-confusion fixes - `dcv_from_any`, `visual_from_interop`, `comp_shape_from_any` and `path_geometry_from_any` now check the vtbl instead of trusting a QI that a catch-all always answers. Was a page fault about one run in seven |
| `dlls-dcomp-device.c.patch` | `window_host_vtbl` extended from 8 to 32 traced slots; MrmCoreR calls slot 17 and the short vtable was a wild jump through `.data` |
| `dlls-windows.storage.applicationdata-applicationdata.c.patch` | `IApplicationData3`, so `GetPublisherCacheFolder` answers instead of leaving a null deref after `Solitaire::Startup START` |
| `dlls-shell32-shellord.c.patch` | `CreateStorageItemFromShellItem_FullTrustCaller_ForPackage`, which is what let XSAPI initialise |

## What is deliberately **not** here

Everything else in that tree's `git status` belongs to the other session. The exporter takes an
explicit file list rather than a glob precisely so it cannot start sweeping their work into our
history the first time they touch a neighbouring file. Per AGENTS.md that includes
`dlls/kernelbase/*`, `dlls/ntdll/{loader,rtl,sync,signal_x86_64,threadpool}.c`, `dlls/win32u`,
`dlls/user32`, `dlls/combase`, `dlls/d3d11`, `dlls/windows.graphics`, `dlls/wintypes`, `server/*`
and `include/*` - and also `dlls/apisetschema/apisetschema.spec`, which they have modified and which
is why the appmodel apiset had to be repaired from `wineuwp` instead of fixed at source.

Two consequences worth remembering: a fix that belongs in one of their files has to be done
somewhere else (the import-thunk repair, the `windows.networking.connectivity` wrapper that lives
in `shims/networkstatus` instead), and **this repo is not sufficient to reproduce the environment
on its own** - it needs that Wine tree at the recorded base plus these patches.
