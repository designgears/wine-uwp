# Working on wine-uwp

Running **packaged UWP apps we have no source for** under a patched Wine. The target is Shapr3D.
Calculator, Feedback Hub, Media Player, Camera and Solitaire are test subjects, in that order of
usefulness - Calculator because it is small and byte-comparable, the others because they are the
same .NET Native + WinUI 2 shape Shapr3D is, and Solitaire because it is the closest proxy for
Shapr3D's *rendering* path.

`docs/Plan.md` has the architecture. `docs/Findings-M*.md` are the running log - **append findings
there**, including the ones that did not work. This file is the part that generalises: how to
investigate, and the mistakes that have already cost time.

## Layout

This repo carries its own data, so a clone plus the two ignored directories is the whole
environment:

| Path | What |
| --- | --- |
| `run.sh`, `install-overrides.sh` | launching and prefix setup: thin host-side wrappers over Wine's `programs/wineuwp-launch` and `programs/wineuwp-setup` (`wine/programs/`). A package is staged with `wine wineuwp-install PACKAGE uwp-bin --windows uwp-bin/win11 [--stage NAME]`, where PACKAGE may be a file, a URL or an installed package's directory (`/mnt/win/Program Files/WindowsApps/<full name>`, which `stage-app.sh` used to copy) |
| `shims/`, `probes/`, `CMakeLists.txt` | the WinRT stand-ins and the diagnostic probes |
| `*.py` | the investigation toolkit (`xref.py`, `winmd.py`, `piid.py`, `fasthive.py`, ...) |
| `wine/` | **our Wine source**, vendored in full by `sync-wine.sh` - 196 files, +49k lines vs upstream |
| `docs/` | Plan, Findings, screenshots |
| `check-clean.sh`, `githooks/` | refuses Microsoft's files (and any big binary) at commit; `git config core.hooksPath githooks` once per clone |
| `uwp-bin/` | **gitignored**, 3.9G: staged apps, the Windows image we borrow DLLs from, PDBs, built shims |
| `prefix/` | **gitignored**, 946M: the live Wine prefix |

`env.sh` defines `UWP_BIN`, `WINEPREFIX`, `WINE_SRC` and `WINE_BIN_DIR`; every shell script sources
it and the Python tools get the same values from `uwppaths.py`. All four are overridable.

**The Wine *checkout* is not in here**, because `~/src/wine` plus `~/src/wine-build` is a large
build tree that predates this repo. Our Wine *source* very much is: `wine/` holds every file this
project has changed, in full, and `sync-wine.sh` moves them either way. All of it is ours - there
is no other owner and no file in that tree is off-limits.

This repo was split out of the Microsoft Calculator fork it started in, keeping the 131 commits
that were ours; the Calculator tree is no longer involved.

## Ground rules

- **Keep this file current as you go, not at the end.** The moment a run teaches you something
  that would have saved you an hour, write it down - a new toolkit entry, a new trap, a corrected
  claim, an app's state changing. Do not batch it up for the end of a session: sessions get
  truncated, and the finding that never got written is the one the next session pays for again.
  The same goes for a line here that has become **wrong** - delete or correct it in the same commit
  that disproves it. A stale `AGENTS.md` is worse than a short one, because it is trusted. The
  `Current state` table at the bottom is the part that rots fastest; update it whenever an app's
  behaviour changes.
- **Always capture a screenshot and put it in front of the user with any progress claim.** Not a
  colour count, not a grep tally, not a log line - a picture. `identify`/`%k` is a cheap *screening*
  metric, never the evidence. And read the screenshot yourself before showing it: 151118 colours and
  "cards on screen" was Solitaire's **splash-screen artwork**, which is decorative and means the
  game has not started. A high colour count proves something painted, not that the app works. Say
  which screen it is.

    ```
    W=$(DISPLAY=:98 xdotool search --name "<title>" | tail -1)
    DISPLAY=:98 xwd -id "$W" -silent > /tmp/s.xwd && magick xwd:/tmp/s.xwd /tmp/s.png
    ```

  Then `Read` the PNG, describe what is actually on it, and embed it in the reply.
- **Every test run kills the user's own session.** The `/tmp` start scripts (`s3cycle.sh`, `s3open.sh`, `regress.sh`)
  run `wineserver -k` on the one shared prefix, so the Shapr3D the user has open on `:0` dies with it - twice in one
  day, the second time while it was the broken state the user had left running for me to inspect. When the user says
  "it's open, check it out", inspect it in place (`xwd -id`, the log, `probe-threads.exe`) and **say before** running
  anything that restarts Wine. **Test in a copy instead:** `cp -a --reflink=always prefix uwp-bin/prefix-test` (btrfs,
  instant, signed in like the original) and `export WINEPREFIX=$PWD/uwp-bin/prefix-test` for every test command - a
  different prefix gets its own wineserver, so `wineserver -k` there leaves the user's app alone.
- **A wrapper that sets `WINEPREFIX` on its `wineserver -k` only runs `run.sh` on the dev prefix**, because `env.sh` defaults it: `/tmp/xbox-test.sh` did that on 10-15 and killed the dev prefix's wineserver three times. Export it for the whole script, and check `/proc/$(pgrep -x wineserver)/environ` when in doubt.
- **A separate prefix does not isolate you from the user's app: the Wine build tree is shared.** Every prefix loads its builtins from `~/src/wine-build`, so an experimental `make dlls/ntdll/all` there goes straight into the user's next launch. On 10-15 an agent's heap instrumentation made the user's Xbox app abort in ReactNative. Build experiments in a reflinked copy (`cp -a --reflink=auto ~/src/wine-build uwp-bin/wine-build-<name>`, then point `WINE_BUILD` and `PATH` at it), and rebuild the shared tree only with a finished, regression-tested change.
- **Xbox install data needs its own override too.** `run.sh` defaults `WINE_UWP_GAMES_DATA` to `uwp-bin/xbox-games` even with a separate prefix. Export an existing isolated data directory for installation/add-on tests; `uwp-bin/xbox-addons-ui-test` contains a small Halo installed-state fixture for the Manage UI. Reuse the existing test prefix/build/data instead of making more copies, and stop trace instances after capture.
- **`run.sh` stops the app after `UWP_TIMEOUT` seconds (60 by default).** Launching an app for the user to use needs `UWP_TIMEOUT=86400`, or it vanishes after a minute. Since 10-21 `run.sh` only picks the Wine, prefix, overrides, timeout and log; `wineuwp-launch --stage DIR --defaults launch-defaults.env` does the rest (identity, framework search path, AppContainer key, CoreMessagingRegistrar), and the app is its child, so a timeout ends the prefix with `wineserver -k`. **A stage needs an `identity.env` naming `UWP_EXE`** (Calculator's defaults used to live in `run.sh`), and with `--defaults` a stage's own `launch.env` is ignored, as `run.sh` always ignored it.
- **Run on Xvfb `:98`, never the user's `:0`.** `DISPLAY=:98 ./run.sh`. **Check that `:98` is yours before
  trusting a capture**: another program took it over on 09-24 with a full-screen window, and every
  Shapr3D capture came back black (1 colour) while the app was fine. `xdotool search --onlyvisible
  --name .` plus `xdotool getwindowpid` names the owner; leave it alone and start your own
  (`Xvfb :97 ...`). The /tmp test scripts take `TESTDISPLAY`.
- **The Wine tree (`~/src/wine`) is ours, all of it.** It is simply a checkout that lives outside
  this repo. Edit anything in it, then `./sync-wine.sh --from-tree` and commit the result here.
  Earlier revisions of this file claimed the tree was "shared with another session" and listed
  `dlls/kernelbase`, `dlls/ntdll`, `dlls/win32u`, `dlls/user32`, `dlls/combase`, `dlls/d3d11`,
  `dlls/windows.graphics`, `dlls/wintypes`, `server/` and `include/` as off-limits. **That was
  false** - every commit in the tree is ours - and it cost real work; see the mistakes section.
- **Halo packs allocator pointers into 43 bits.** If its crash dump faults at `HaloInfinite.exe+0x68c821` with an unpacked `0x7...` pointer and a freelist structure at `0x7f...`, inspect allocation placement first. After exhausting preloader reservations, anonymous Unix mmap returns high Linux addresses; preserving bottom-up allocation with `map_free_area` gets past this crash. A leftover `crash_report/minidump.dmp` is not evidence of a new crash: compare its timestamp with the current launch.
- **A GDK package identifier is at most 32 characters, not the package full name.** Halo's campaign asks
  `XPackageGetCurrentProcessPackageIdentifier` with a 33-byte buffer; refusing it silently disabled every availability
  query and it then read optional HD1 modules that were never installed (`ModuleSizeOnDisk: 0` halt). The identifier is
  the MSIXVC content id in hex (`.wine-uwp-chunks.json` `"identifier"`, or `layout_<id>.xml`). When a game "makes no
  XPackage call" you expected, look for a call that *failed earlier* in its init, not for a different API. Halo installs
  without HD1 are correct; do not install optional features to get past a content halt.
- **To test a game "from nothing", reflink its installed stage, do not download it again.** `cp -a --reflink=always`
  copies Halo's 75 GB in 0.3 s; strip hand-made additions and regenerate `launch.env` with the installer's code. A fresh
  prefix (`packaging/new-prefix.sh`) finds bugs a long-lived one hides: 10-19 found four (Findings-M4).
- **GDK runtime semantics are load-bearing, not cosmetic.** Halo's queues are all Manual: an XTaskQueue that runs
  callbacks inline corrupts the title's allocator from foreign threads. XAsyncBegin must accept `E_PENDING` from a
  provider's Begin (libHttpClient's WebSocket send returns it). A title's partner services need the title's own
  endpoint table (`title.mgt.xboxlive.com/titles/<tid>/endpoints`, authorised, no query string). When a GDK game
  misbehaves on the network, read its WebSocket/AMQP payloads before theorising: Halo's telemetry stream carries its
  own log lines.
- **`launch.env` is not a shell script to `wineuwp-launch.exe`.** Its parser removes surrounding quotes and sets literal values; it does not expand `${VAR}` or execute shell syntax. Generate complete literal override lists rather than appending a variable reference, even though `run.sh` can source the same file as shell code.
- **A child's host environment used to be a copy of its parent's, whatever block `CreateProcess` passed.** Wine reads
  `WINEDLLOVERRIDES`, `WINE_HACK_NATIVE_SYSCALLS` and `WINE_UWP_*` with `getenv`, so `launch.env` values set by
  `wineuwp-launch` reached only the game's Windows environment and were lost when EAC's native helper started the game
  ("Package exe running outside of a package"). `spawn_process` now copies those from the block into the child's host
  environment (`sync_unix_env`), and the old per-exe `AppDefaults` workaround is not needed.
- **Never push.** Commit freely; pushing is the user's call.
- **Nothing of Microsoft's goes into git, and nothing of Shapr3D's product goes into this repo.** No Windows DLLs, app
  packages or their payload (Calculator, Media Player, Solitaire... live only in gitignored `uwp-bin/`), no Windows
  fonts, images or metadata: `check-clean.sh` enforces it. The Shapr3D product - the Slint installer, the AppImage and the Flatpak
  - is its own repo, `../shapr3d-linux`, built against this checkout (its `WINE_UWP`). wine-uwp
  stays generic: Wine, `wineuwp`, shims, tools and the runtime scripts under `packaging/`.
- **Stay in this directory.** The only path outside it that is ours is the Wine checkout
  `$WINE_SRC` (`~/src/wine`), because `env.sh` and `sync-wine.sh` point at it. Everything else
  under `~` is unrelated work - do not read it, scan it, or report on it unless asked. I once ran a
  `git status` sweep over every repo in `~/src` and `~/Projects` while answering "is anything
  uncommitted?"; the question meant *this project*, and the other repos were none of my business.
- **Sync our Wine source into this repo after touching it**: `./sync-wine.sh --from-tree`, then
  commit. `--check` tells you whether the two are in step; `--to-tree` reconstructs the tree from
  the repo. "Commit only files in this repo" is not the same as "let our Wine work exist nowhere":
  it is **+49,264/-1,024 lines across 196 files**, and 29 of them - `dlls/dcomp/composition.c`
  (14k lines), `dlls/wineuwp/main.c` (4.8k), `dlls/dxgi/compswapchain.c` - **do not exist upstream
  at all**. They are our source, not patches.
  Commit in the Wine tree too - it is ours, and it is where `git log` for that work lives. The
  repo copy is what makes it visible and recoverable; the two are not alternatives.
  `wine/FILES` is an **explicit list, never a glob**, so build output and stray files never get
  swept into our history.
  `--check` also fails on files changed in the tree but **missing from the list** - it found 16,
  including 41 committed lines of `appxdeploymentclient/package.c` that had never been vendored.
- **This used to be a patch export, and it silently lost ~95% of the work.** Two compounding
  causes: `git diff` sees only the *uncommitted* delta, while most of the work was already
  committed in that tree; and the exporter's file list came from the bogus "not yours" claim
  below, dropping another ~5,000 lines. It captured 2,700 lines and looked complete. If you ever
  find yourself exporting diffs of our own code again, stop.
- **Byte patches go in `$UWP_BIN/win11/...`**, but the app loads the prefix copy, and nothing copies it
  there on a run (`install-overrides.sh` copies only when its stamp changes, some DLLs only if
  missing). Patch both copies. Always keep a
  backup and verify the restore with `cmp` before moving on. Leaving one behind silently poisons
  every later run - it has happened.
- **A DLL variant under test goes into both copies.** `install-overrides.sh` copies `$UWP_BIN/win11/...` over a
  prefix copy that differs from it whenever its stamp changes, and changing the `win11` file changes the stamp. A
  variant written only to the prefix was silently replaced; `WINE_UWP_BP`'s `BP armed ... (byte xx)` line shows which
  bytes were really loaded.
- **Which patches are still load-bearing is measured, not remembered.** Findings-M4 2026-09-28 bisected all three
  patched DLLs one region at a time, and then retired them in Wine. Live today:
  - **CoreMessaging.dll, Windows.UI.dll and Windows.UI.Xaml.dll are all the pristine 26100.8972 files.**
    `./pecheck.py --sweep uwp-bin/win11` lists only backups.

  The earlier variants are kept as `.bak-minimal`, `.bak-kbonly`, `.bak-1patch`, `.bak-full` and `.bak-18regions`. The CoreMessaging
  patches, the Windows.UI `ProcessMessage` trampoline, XAML's `ShouldTryLoadAppXaml` and combase's 16 ms poll were all
  one missing wake: the thread's CoreMessaging completion port waking its message wait (win32u, "CoreMessaging and
  Windows.UI.Xaml run unmodified"). The key `Handled` byte was a missing text input producer (our builtin
  `windows.ui.core.textinput` ordinal 1500). Every patch stands for a Wine-side bug, and fixing that bug is what
  retires the patch.

  **The four-app suite cannot judge a keyboard patch** - run `./regress.sh kb` too. **Nor can it judge a start-up
  race**: a 1-in-5 stall passed two suites. Count 20 Calculator starts before adopting a change to start-up.
- **Rebuilding `wineuwp` (or anything install-overrides stamps) copies the `win11` DLLs back over the prefix.** A test
  with a variant only in the prefix silently reverts the moment you rebuild. On 09-28 that turned a passing run into
  blank Calculator, Feedback Hub and Media Player: the patched XAML came back, and `wineuwp` no longer loads App.xaml
  for it. Adopt a variant into `win11` before rebuilding, or re-copy it after, and `cmp` the prefix copy after the run.
- **A byte patch's "free BSS" may not be free.** The CoreMessaging timer patch kept its mailbox
  and a thread id at `+0x10d000..+0x10d010`, which are wil's error counters and a cached function
  pointer, so the two overwrote each other. Look an address up in the PDB `.syms` before claiming it.
  `HotPatchSpareGlobals` is the space Microsoft reserves for exactly this.
- **Never patch the last pristine copy of a Windows DLL.** None is patched today. CoreMessaging, Windows.UI and
  Windows.UI.Xaml (26100.8972) were until 09-28, and all three originals are
  kept as `<name>.pristine-26100.8972` and **cannot be re-obtained** - the tree is a mix of builds,
  and the machine they were copied from has serviced past them. `./pecheck.py` tells a pristine
  Microsoft PE from a patched one with no reference copy. Check before patching, and never delete a
  `.pristine-*`.
- Commit messages end with the attribution line the harness gives you.

## Moving this tree

The data directories live inside the repo now, but a handful of things bake absolute paths and will
silently break if it moves again. All of these bit me during the move out of the home directory:

- **`"$HOME"/x` does not match a `$HOME/x` search.** A sed over `\$HOME/uwp-bin` and `~/uwp-bin`
  left `"$HOME"/uwp-bin/stage/frameworks/*/` in `run.sh` untouched, because of the quotes. The glob
  then matched nothing, so the loop ran **once with a literal `*`**, and the app was handed a
  framework dependency path containing a wildcard. Grep for `HOME.*uwp-bin` - loose, not exact -
  and check `${HOME}` and `"$HOME"` variants before believing a rewrite is complete.
- **The prefix registry holds ~2,500 absolute paths.** Rewrite them with wineserver **down**, or
  the server flushes its in-memory copy back over your edit.
- **`.reg` files cannot expand variables**, so any path in one is baked to whoever generated it.
  `wineuwp.reg` now carries an `@WIN11_WINPATH@` placeholder (the Windows tree) that `install-overrides.sh`
  substitutes at import time.
- **`build.sh` fails with a stale CMake cache after a move.** `uwp-bin/build-probes/CMakeCache.txt`
  records the *old* source and binary directories (`/home/bobby/Projects/calculator/tools/wine-uwp`,
  `$HOME/uwp-bin/build-probes`) and CMake refuses to reconfigure. `rm -f
  uwp-bin/build-probes/CMakeCache.txt && rm -rf uwp-bin/build-probes/CMakeFiles` then `./build.sh`.
  Back up `uwp-bin/build-probes/*.exe` first if you are mid-investigation - they are the only copies
  until the rebuild finishes.
- **`identity.env` in a stage directory must not contain `WINE_UWP_PACKAGE_PATH`.** `wineuwp-launch` (formerly
  `run.sh`) reads that file, so a stale value there would win over the computed one. The launcher always computes
  it from the stage directory and the installer does not write it.

**`info[0]=0x8007007B` in an exception record is `ERROR_INVALID_NAME`** - "the filename, directory
name, or volume label syntax is incorrect". It means a path is *malformed*, not missing, which is a
much narrower thing to look for: an unexpanded glob, a doubled separator, an empty segment. Trace
with `WINEDEBUG=+file` and read the last named `CreateFileW`. That one number turned a day-shaped
hunt into a ten-minute one, after I had already wasted four runs checking that paths *existed* -
they all did.

## The regression protocol - do not skip it

Every change to Wine or the host gets checked against apps that already work, *before* you believe
a result:

**The baseline lives in the repo, at `docs/baselines/calc-regress.png`** - a 1600x1000 *root*
capture. It was re-taken on 2026-09-23 when `IDWriteFontFamily2::GetMatchingFonts` was implemented:
XAML's text then takes its normal font path instead of an E_NOTIMPL fallback, which moves text by
one pixel (3486 pixels differ). The change was confirmed to come from dwrite alone (the old dwrite
gives AE 0), and the new rendering is identical across runs. Re-baseline only like that: attribute
the difference to one change, look at it, and run twice. Re-taken on 09-24 for dcomp re-reading textures XAML writes directly (134 px, the "=" border; confirmed by toggling), and before that on 09-24 for dwrite's optical size (192 px: only the 12 px title caption, now in Segoe UI Variable's Small design). Re-taken again on 09-23 for dwrite's
subpixel glyph positioning (771 pixels, antialiasing inside glyphs; layout identical), and again for
linear gradient brushes (950 pixels, the buttons' elevation borders now drawn). It used to live only in `/tmp`, and `/tmp` gets wiped: the baseline, every session log and
**the Xvfb socket for `:98`** all vanished at once. Nothing starts `:98` automatically, so the first
symptom was every app failing activation with `PrivateCreateCoreWindow failed, hr 0x80004005` -
which looks like a Wine regression and is not. **If a run suddenly fails at activation, check
`DISPLAY=:98 xdpyinfo` first**, and restart the display with
`Xvfb :98 -screen 0 1600x1000x24 -nolisten tcp &`.

**Run it with `TESTDISPLAY=:97 ./regress.sh`** (or name apps: `./regress.sh calc mp`), **plus `./regress.sh kb`** (on `:97`: at `:96`'s 3440x1440 it differs from its baselines by design) after anything touching input or Windows.UI: Calculator `12+3` Enter / Backspace `7` / Escape against `docs/baselines/calc-kb-*.png`. It polls each
app once a second and moves on when it passes, so the suite takes under 2 minutes. Do not go back to
fixed sleeps; the apps are up in 3-6 s. What it checks, by hand:

```
# Calculator must stay identical to the saved baseline
DISPLAY=:98 UWP_TIMEOUT=80 UWP_LOG=/tmp/c.log ./run.sh &  sleep 55
DISPLAY=:98 xwd -root -silent > /tmp/c.xwd && magick xwd:/tmp/c.xwd /tmp/c.png
magick compare -metric AE /tmp/c.png docs/baselines/calc-regress.png null:   # 0 = identical

# Feedback Hub: no fail-fasts, no app error, ~822 unique colours
DISPLAY=:98 UWP_STAGE=windowsfeedbackhub ... ; identify -format '%k' /tmp/fh.png
```

`identify -format %k` (unique colours) is the cheap "is it painting" metric: 1 = blank, hundreds =
real UI.

**Run anything flaky twice.** A change that produced 66 fail-fasts in one run and 6 XAML
activations in the next looked fine in its first run. Single runs are not evidence.

## Investigation toolkit

These all exist and work. Prefer them over reading code and guessing.

**Run the Python tools with `$UWP_BIN/venv/bin/python`, not `python3`.** Five of them import `pefile`,
which is installed only in that venv: `disas.py`, `import-audit.py`, `ordinal-audit.py`, `pdbsyms.py` and
`xrefs.py`. Under a bare `python3` they die with `ModuleNotFoundError` and look broken. The prefix
builders are C now (Wine's `wineuwp-setup` generates every `.reg` from the hive; the `gen-*-reg.py` scripts are gone,
10-21), and `packaging/` needs nothing but the standard library. This file previously asserted that `pefile` was
"not installed", advising you to hand-parse PE headers instead - that was wrong, and it cost a
session exactly that detour. `requirements.txt` records the dependencies; the venv lives inside
gitignored `uwp-bin`, so a fresh clone has to recreate it.

| Question | Tool |
| --- | --- |
| What is this IID, and what are its methods? | `./winmd.py <winmd> --guid <iid>` (public interfaces; prints slots from 6) |
| Demangle an MSVC symbol | `llvm-undname` |
| Symbols for a Microsoft binary | read the RSDS record from the PE debug directory, `curl` it from `msdl.microsoft.com/download/symbols/<name>/<sig><age>/<name>` ([docs](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/microsoft-public-symbols)), then `llvm-pdbutil dump --publics` into a `rva name` `.syms` file next to it in `$UWP_BIN/pdb`. `./pdbsyms.py` does this - run it with `$UWP_BIN/venv/bin/python` (see below) |
| Which module is this address in? | `./addr2mod.py <log-with-+loaddll> <addr>` - **native modules only**, see below; for a Wine builtin use `/proc/<pid>/maps` |
| Which of N call sites returned this HRESULT? | unique-HRESULT tagging, below |
| Did this code path run at all? | patch `0f 0b` (`ud2`) over its first two bytes, below |
| What did managed code throw, and where? | `WINE_UWP_TRACE_ORIGINATE=1` + the chunked `trace_language_exception` in `dlls/combase/roerror.c` |
| How many real `CoreApplicationView`s exist, on which threads? | `WINE_UWP_REPORT_VIEWS=1` (read-only census) |
| Does the app's viewport reach the screen, and with what gaps? | `WINEDEBUG=+timestamp,trace+dcompgate` then `./shownframes.py <log>`: swap chain frames *shown* per 250 ms after the viewport starts, and the worst gaps. Presents and paints can both look fine while no frame is shown |
| Where does a present cycle go, the app's time or ours? | `WINEDEBUG=+timestamp,trace+d3dperf`: every composition swap chain present with the time since the last one (the app's) and inside it: vblank, copy and `notify` (our back-pressure; 20 ms is its timeout). Waiting in `notify` means the painter is the limit |
| Does a D3D operation store what it should in an A8 (or other odd) format? | `probe-a8.exe` clears, uploads, box-copies from staging and D2D-fills a 9x9 `A8_UNORM` texture and reads each back. Run it with `WINE_D3D_CONFIG=renderer=vulkan` - a bare probe gets the GL renderer, the apps get Vulkan from `run.sh` |
| Is it rendering more than it should? | `WINEDEBUG=+dcompperf`: one line a second with presents, readbacks and painted frames. More presents than the refresh rate means the app is not being paced |
| Which paint ops did a frame emit, and what did the painter skip? | `WINEDEBUG=+dcompops`: every emitted op with dest, clip, alpha and quarter turns, every `Commit`, and every visual skipped as hidden or transparent. Far lighter than `+dcomp`, which filled a 16 GB `/tmp` in one menu-open trace |
| Is this visual in the tree at all, and where is it painted? | `WINEDEBUG=+timestamp,+dcomptree`: every visual create, insert and remove, and every painter visit with position, clip, offset/transform fields and scale. `./dcomptree.py <log> <visual> [time]` rebuilds the ancestor chain from it and prints each ancestor's last painted position. It found a menu that was built, in the tree and painted every frame - at y = 1.5e29. `WARN`s `is not a coordinate` (`warn+dcomp`) name the caller of any absurd offset |
| How long does the app take to react to a click? | press and release by hand, `xdotool mousedown 1; sleep 0.02; xdotool mouseup 1`, and grab raw `xwd -id` frames in a loop (2 ms each; compare afterwards). **`xdotool click` waits ~100 ms before the press**, so every latency measured with it reads ~105 ms |
| What is the visual tree doing? | `WINEDEBUG=+dcomp`; `present_desktop_hwnd frame: N ops` - 0 ops means nothing to draw |
| Which thread is spending the CPU in an interaction? | `/proc/<pid>/task/*/stat` (utime+stime) before and after; Wine names the Linux threads (`KernelRunLoop`, `ApplicationView`, `Render thread`, `wine_threadpool`). For *when*: `./schedtl.py <pid> <s> <out>` samples every thread's `schedstat` every 5 ms, `./schedview.py <out> [t0 t1 N]` prints a 10 ms-per-character busy strip per thread. That showed Shapr3D's UI thread saturated by our painting, and the kernel's compute/wait rhythm |
| Symbolize a `WINE_UWP_PROFILE` dump | `$UWP_BIN/venv/bin/python profsym.py <dump>` (threads by name), `profsym.py <dump> <tid> [N]` (top leaves with real 8-frame unwound callers; `DEPTH=n`), `BUSY=1` to drop samples parked in waits. Profile at 5 ms: 3 ms with ~80 threads crashed Shapr3D once |
| Is something creating shaders every time the UI changes? | `WINEDEBUG=+timestamp,trace+d3dperf`: every `d3d11_device_Create{Vertex,Pixel}Shader` with its caller, and every wined3d CPU blit (`cpu blt`, source and destination access and locations). A menu open created ~1200 shaders from d2d1 until contexts shared them |
| How smooth is a menu or popup transition? | act with the uptime logged (`cut -d' ' -f1 /proc/uptime`), run with `+timestamp,trace+dcompgate`, then `./transitions.py <log> <times>`: first frame, changed frames, largest gap, gate stalls. `frame done` lines give each frame's walk, raster and damage. Check positions with one `xwd` per open at fixed delays, never a tight loop |
| Is something creating D3D devices? | `WINEDEBUG=+timestamp,trace+d3dperf`: every `D3D11CreateDevice` with its time, thread and caller, and every device destroyed. Win2D made 100 in 11 s on Shapr3D's render thread, 72 ms each |
| What did a thread do between two moments? | copy the `WINE_UWP_PROFILE` dump at both moments and `./profdiff.py before after diff` (cumulative counts subtracted), then `profsym.py diff` |
| Reproduce at the user's refresh rate | `WINE_UWP_TEST_HZ=175` paces dcomp's painter and dxgi's vblank grid as a 175 Hz display would; Xvfb reports 60. The wheel-over-panel leak was 10 in 150 at 175 Hz and never showed at 60 |
| Which offset did XAML scroll to, and who asked? | `WINE_UWP_BP` on `ScrollViewer::ChangeViewInternal` (`windows.ui.xaml.dll+406ac8`, `ret` names the caller) and `BringIntoViewportInternal` (`+405ba4@rdx+0:4`, the target rect as floats); `trace+manipulation` shows what reaches DirectManipulation (`set to scale ... offset`, `scrolled to`) |
| Reproduce at the user's window size | the user's screen is 3440x1440: `Xvfb :96 -screen 0 3440x1440x24`, then `wine $UWP_BIN/build-probes/probe-resize.exe 3440 1400 Shapr3D`. Per-frame painting costs scale with it, and a 1280x800 run hid a UI thread at 100% |
| Why is an app stuck, when the stack dumper cannot say? | `WINE_UWP_PROFILE=<prefix> WINE_UWP_PROFILE_MS=10` - a sampling profiler already in `wineuwp`. Writes `<prefix>.<pid>` every 2 s: an `M` line per module then `S <tid> <rip> <count> <4 callers>`. It **found the waiting thread's loop when `WINE_UWP_DUMP_STACKS` returned nothing useful**, and it distinguishes *blocked* from *spinning*, which no single stack snapshot can |
| What is every thread actually doing? | `WINE_UWP_DUMP_STACKS=<seconds>` - suspends and unwinds every thread, prints `STACK tid N frame M rip=`. Works on an *idle* app, not just a hung one. Resolve against `/proc/<pid>/maps` from the same run |
| What is a thread of a *stuck* process doing, from outside, with no restart? | `wine $UWP_BIN/build-probes/probe-threads.exe <exe> all` - cross-process `SuspendThread` + `GetThreadContext` **works in Wine** although ptrace does not: it lists every thread by Wine tid and description with three register samples, so "blocked by 03cc" gets a rip. `probe-peek.exe <exe> <addr> <qwords>` reads the process's memory. Resolve rips against the `M` lines it prints and `llvm-symbolizer --obj=<build dll>` (image base from `llvm-objdump -p`). This found the corrupted `futex_queues` lock in one live process after days of flushed-log inference |
| Who writes into freed heap memory? | `WINEDEBUG=warn+heap`: ntdll delays 16384 frees with the blocks filled (LFH off) and reports `block X (size S) written after free at +off: <value>; freed from <4 frames>` when one leaves the ring. The value is the clue: a vtable or static address names the writing module. `probes/probe-uaf.cpp` is the control. Heap corruption otherwise surfaces far from its cause, as a crash inside `RtlAllocateHeap` |
| Is our Xbox Live request signing right, with no account? | `probe-xboxauth.exe sigtest` (xal-rs' test vectors through BCrypt) and `devicetoken` (a real device token from `device.auth.xboxlive.com`, with unsigned and bit-flipped controls that must fail). **Only device/authenticate is a usable control**: XSTS with a device token alone accepts a damaged signature. Run from `uwp-bin/build-probes` with `WINEPREFIX` set and a bash script - in zsh `$m` does not split `"devicetoken Win32"` |
| Does the app's crash reporter work? | `WINE_UWP_TEST_CRASH=<seconds>` faults on a thread of `wineuwp`'s after that long; Shapr3D's Sentry should leave a `.dmp` in `LocalState/crash_report/reports` |
| Parameter types, struct fields, enum values of a WinRT type | `./winmd.py <winmd> --sig <Namespace.Type>`: signatures with names and `out`, which `--guid`/`--type` do not show |
| Method count of a private interface | parse the MIDL proxy/stub DLL, below |
| A parameterised IID over `String`/`Object`/`Int32` | `./piid.py` does **not** know base types: `IVector<String>` silently hashes `String` as a type *name* and prints a wrong IID. Use `--signature` with `string`, `i4`, `cinterface(IInspectable)`: `pinterface({913337e9-11a1-4345-a3a2-4e7f956e222d};string)` gives `98b9acc1-...`, the known `IVector<String>` - check a control like that first |
| Did this code path run, and with what registers, without ending the run? | `WINE_UWP_BP=windows.ui.xaml.dll+35dcd0,<module>+<rva>,...`: int3 breakpoints planted by `wineuwp` once each module loads. A vectored handler, which runs before the target's own handlers, logs `BP <module>+<rva> hit N tid rcx rdx r8 r9 rax ret=<module+off>`, single-steps over the breakpoint and re-arms it. `WINE_UWP_BP_MAX` caps the lines per breakpoint (default 20). **Use this before `ud2` inside XAML**: XAML swallowed `ud2` probes and gave contradictory negatives, while this showed the whole LoadedImageSurface chain in one run. The address must start an instruction. **Append `@reg+off:n` to also dump `n` dwords at that address as hex and float** (`windows.ui.xaml.dll+449e26@rbp-60:4` printed XAML's visible-bounds `Rect` straight after the call that filled it); registers: rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r12-r15. That is how a value computed *inside* a Microsoft function gets read without a trampoline. **`@stk:n` prints the caller chain instead**, `n` frames unwound through `.pdata` as `module+offset` - it named the app's `PointerExited` handler behind a `GoToState` in one run, through .NET Native frames. One dump per address: a second breakpoint on the very next instruction broke the single-step and fail-fasted the app. Long dumps print eight dwords a line; one Wine debug line holds ~1 KiB and overflowing it aborts the process |
| Did this code path run, without ending the run? | `./tramp.py <exe> <rva> <nbytes> 'MSG' [cave] [--applog]` - a jmp to a code cave that logs and jumps back, so one run answers several questions. It appends its own RWX `.tramp` section, refuses a displacement that splits an instruction, and relocates displaced rip-relative operands |
| What is the value of a register or field at that point? | `./tramp.py ... --val=rcx` / `--val=[rcx+0x30]` / `--val=[[rcx+0x30]+0x14c]` - formats it as hex through `OutputDebugStringA`; read it with `WINEDEBUG=warn+seh` |
| Where is this class's vtable, with no symbols? | `./rtti.py <exe> '<name substring>'` - walks the MSVC type descriptors and prints the vtable and its slots |
| What function is this address in, with no symbols? | `./whatfn.py <exe> <addr>...` - finds the enclosing `.pdata` function and prints every string its rip-relative LEAs point at. That is how a bare stack dump gets names |
| Where is the code behind one of the app's own log strings? | `./xref.py <exe> --string '<log string>'` - finds rip-relative LEAs to it and resolves each to its **enclosing function** via `.pdata`, which is where a `ud2` has to go. `--callers <rva>` finds direct call sites |
| Does a size change reach the app? | `$UWP_BIN/build-probes/probe-resize.exe <w> <h> [title]` - `SetWindowPos` through Wine. **`xdotool windowsize` does not reach Wine at all** |
| Is this staged app binary still pristine? | `./appxverify.py <stage-dir> [file...]` - checks every 64 KiB block against the package's own `AppxBlockMap.xml`. An appx payload has no Authenticode signature and no PE checksum, so `pecheck.py` cannot answer this; use it instead of trusting a backup copy |
| Is this Windows DLL one we have byte-patched? | `./pecheck.py <file>...`, or `./pecheck.py --sweep uwp-bin/win11` to list every modified file. Authenticode + PE checksum; needs no reference copy |
| Which .NET Native call site made the WinRT call that failed? | `WINE_UWP_TRACE_ORIGINATE=1` now also prints `origin frame N: module+rva` for each `RoOriginateLanguageException`; the managed caller is still on the stack. Disassemble the frame before `Stub_N` for the slot (`r8d`) |
| Why did the app vanish with exit status 0? | `WINE_UWP_TRACE_EXIT=1`: backtraces for `RtlExitUserProcess`, `TerminateProcess` (any target, with both pids), `TerminateThread` and ucrtbase `abort()`. `WINE_UWP_TRACE_THREAD_EXIT=1` adds every thread exit and `NtTerminateThread`. `WINE_UWP_TRACE_CLOSE=<hex>` names who closes a handle |
| Where did a Sentry/crashpad minidump crash? | `./minidump.py <file.dmp>` - exception, module+offset, and a module-resolved scan of the faulting stack. Wine builtins carry DWARF: `llvm-symbolizer --obj=~/src/wine-build/dlls/<dll>/x86_64-windows/<dll>.dll 0x180000000+<offset>` names the function and line. That is how the dcomp use-after-free and the d3d11 NULL write were found in minutes |
| What is Shapr3D itself logging? | `sqlite3 LocalState/telemetry.db "select data from Records"`: its structured log (file, line, message) sits there until it is uploaded. For any app, request bodies can be seen by temporarily dumping `lpOptional`/`HTTPREQ_WriteFile` buffers in `dlls/wininet/http.c`; the native `Windows.Web.Http` runs on WinINet |
| The app ships a crash reporter (Sentry/crashpad) | read its minidump and breadcrumbs, in `LocalState/crash_report/` for Shapr3D: the dump's exception stream names the thread and address, the breadcrumbs the app's own error text |
| Open a URI or file from an app without touching the real desktop | `WINE_UWP_LAUNCHER=log` makes Wine's `windows.system.hostlauncher` log instead of `ShellExecute`; `probe-launcher.exe` exercises it without an app |
| Deliver a URI to the running app (sign-in callbacks, `scheme:` links) | `./activate-uri.sh <uri>` - a Protocol activation of the running instance, via the inbox `wineuwp` watches. `./browser-callback.sh <scheme>` does a whole browser sign-in and delivers its callback |
| Does a drag reach its drop target? | `WINEDEBUG=+timestamp,trace+dataexchange`: every `TargetRequested`, `EnterAsync`/`OverAsync`/`DropAsync` with the position and the operation the target accepted. Drag with a move every ~15 ms (`/tmp/dodrag.sh`), and only in a project of our own |
| Which visual state did the app switch a control to, and when? | `WINE_UWP_BP=windows.ui.xaml.dll+18c80@r8+0:6` (`VisualStateManagerFactory::GoToState`): .NET Native passes reference HSTRINGs, so dwords 4-5 are a pointer into `Shapr3D.dll` and dword 1 the length - read the literal from the DLL image. `+111338@r8+0:2` (`CEventManager::RaiseRoutedEvent`) gives the routed-event stream with each source's vtable. An app's own XAML is in `resources.pri` as `XBF` blobs with UTF-16 string tables |
| Does a window flash anything before its real content? | `probes/popwatch.c` (host X11 tool, `gcc -O2 -o popwatch probes/popwatch.c -lX11`): logs every top-level create/map/configure/unmap with its depth, and after each map or resize grabs the window's pixels every ~3 ms, counting transparent, opaque-black, opaque-white and other pixels and saving the frames raw (`/tmp/popwatch/w<id>_NNN.bgra`). Alpha is what a compositor shows, so this sees on Xvfb what KWin would. It found XAML's windowed menus mapped as an opaque 24-bit window for 13 ms, and a reopened menu flashing its last frame |
| Build a prefix with no Windows install | `packaging/harvest/harvest.py ISO OUT` extracts `packaging/harvest/manifest.txt` from a Windows ISO's `install.wim` with `7z` (no root). Microsoft's Evaluation Center ISO downloads without a fraud check: `go.microsoft.com/fwlink/?linkid=2334167` (the consumer download page rejects scripts). `packaging/new-prefix.sh TREE PREFIX` builds a prefix from nothing out of a `win11`-shaped tree: services, registrations, symbols, overrides. Run apps on it with `WIN11=TREE` exported, or `install-overrides.sh` copies `uwp-bin/win11` back in |
| Does the whole product work from nothing? | `packaging/` (see its README): `build-dist.sh` makes an installed tree; the installer and the AppImage are built in `../shapr3d-linux` (`build-dist.sh`, `appimage/build.sh`, and `release.sh` for a versioned, smoke-tested release). With `WINE_UWP_DATA` pointing at an empty directory, run `bin/wine-uwp setup [--iso FILE] [PACKAGE]` (the Rust installer, in a shapr3d-linux tree; arguments pre-fill the choices, so no file dialog is needed), or the CLI steps `harvest`, `new-prefix` and `install`. The ISO is not kept (7 GB; `download_iso.py` fetches and verifies it in ~2 minutes); its harvest is, in `uwp-bin/win11-iso` (26100.5074 DLLs, frameworks included), which is also the second Windows build to test against. A synthetic `.msixbundle` from `packaging/make-test-bundle.py uwp-bin/stage/shapr3d OUT` (every file in its `AppxBlockMap.xml`) stands in for the real download; `appxverify.py` then checks the install |
| Which Windows DLL of a build breaks an app? | swap the differing DLLs between two trees and bisect: `/tmp/bisectdlls.sh` in the 09-28 session, 447 DLLs in nine runs. It found `twinapi.appcore.dll` (our hard-coded RVAs) and `Windows.UI.Xaml.dll` (a latent dcomp bug) |
| Private addresses inside a Microsoft DLL, for any build | `packaging/harvest/resolve_symbols.py SYSTEM32 OUT` reads each DLL's CodeView record, fetches its PDB from `msdl.microsoft.com` and parses the publics itself, writing `C:\windows\wineuwp-symbols.txt`. In `wineuwp`, use `uwp_symbol(dll, name)`: **never a fixed RVA** |
| What is the app itself logging? (ETW TraceLogging, wil errors) | `WINEDEBUG=trace+tracelogging` (a long string continues on `  +160 ...` lines, up to 8 KiB): ntdll enables every ETW provider at verbose level and decodes each TraceLogging event (provider, event name, fields). The Xbox app's own `LogMsgError` lines named every blocker after the splash, with source file and HRESULT, where originate traces had only addresses. Very chatty (React Native's Systrace: 60k lines a run); grep the provider you want |
| Does an app's own WinRT interface cross processes? | `WINEDEBUG=trace+winmdps`: the metadata-based marshaler (`dlls/rpcrt4/ndr_winmd.c`, CLSID `{00000355}`) logs every proxy/stub it builds from `.winmd` with method and format-string sizes, and a FIXME for any method it cannot marshal |
| Replace a package's own WinRT class with a stand-in | register it under `ActivatableClassId\<class>` with `OverridesPackage`=1 (DWORD): combase then prefers it to the package manifest, which otherwise wins. Make an absent platform package "installed" with `HKLM\Software\Wine\WineUWP\StandInPackages\<family>` (appxdeploymentclient `standin.c`) |
| Does a Windows DLL that imports by ordinal reach the right function of ours? | `uwp-bin/venv/bin/python ordinal-compare.py uwp-bin/win11/Windows/System32 <build dir>`: every by-ordinal import from a Windows DLL into a DLL we build, with the name Windows exports at that ordinal and ours. A name mismatch is a silent wrong call: uxtheme 121 was our `GetThemeAnimationProperty`, which wrote into the Microsoft Store's service container. `MISSING` means a raising stub |
| Who writes this word of memory? | `WINE_UWP_WATCH=module+rva`: a hardware write watchpoint (DR0) on those 8 bytes on every thread, new ones picked up every 20 ms; each write logs the new value and the writer's caller chain. It found the uxtheme ordinal bug in one run, after the app's only writer of the slot had been ruled out |
| Which methods of an interface does the app actually call? | give every slot a traced thunk returning `E_NOTIMPL` and read the log - see `shims/storecontext`, which proved the app wanted only `GetDefault` out of 26 methods |

### Unique-HRESULT tagging

To find which of hundreds of sites produced an HRESULT: rewrite every `mov reg, <hresult>` in a DLL
to a distinct value (`base + index`), run once, and read the index out of the error message. One
run identifies the exact function. `/tmp/tag.py` in a session is ten lines; the pattern is in
Findings-M4.

**Safe for `E_FAIL`. Not safe for `E_NOTIMPL` or `E_NOINTERFACE`** - callers test for those
specifically and take a different path when they get something else. Tagging XAML's 629 `E_NOTIMPL`
sites took a run from 46 XAML activations to 6. Find those from the FIXME lines our own stubs print
instead.

### ud2 probes

**A hit appears in one of TWO forms, and you must grep for both:**

- `wine: Unhandled illegal instruction at address <addr>, starting debugger...` - nothing caught it.
- `err:seh:NtRaiseException Unhandled exception code 1007` - the app's own fail-fast handler caught
  it first, so the run ends in a `RaiseFailFastException` unwind instead.

- `err:wineuwp:do_activate Activate failed, hr 0xc000001d` - the probe fired during activation and
  came back as `STATUS_ILLEGAL_INSTRUCTION`. Some callers go further and swallow the failure
  entirely: a `ud2` in `WindowServer::Activate` showed no hit at all while that function was
  running in every good run. **A function whose failure its caller ignores gives a false negative**;
  trace it instead (return-address logging, a vtable hook).

Which one you get depends on the app, not on the probe. I grepped for only the first and read a
*fired* probe as a miss; this file previously documented only the second and did the same in
reverse. `grep -aE "illegal instruction|code 1007"`.

**Validate the probe target on an app that already works**, not just the probe mechanism. Those are
different claims: a firing control proves `ud2` works in that process, not that the address is the
function the app actually calls. Calculator and Media Player both fire
`WindowGenerated::put_Content`, which is what makes Solitaire's silence there mean something.

**Do not name a probe helper function after a shell alias.** A helper called `c()` failed with
`defining function based on alias 'c'` - a *parse* error, so the whole command aborted. It happened
to abort before patching, but the restore lives at the end of that same command, so a parse or
runtime failure after the patch leaves a `ud2` in the binary. After any probe command that exits
non-zero, `cmp` the target against its pristine copy before doing anything else. Use distinctive
helper names (`runprobe`, not `p`/`c`/`r`).

**Patch and restore *every* copy.** The app loads `$WINEPREFIX/drive_c/windows/system32/`, not
`$UWP_BIN/win11/...`: a `ud2` in the win11 copy alone never fires (a control probe caught this). And a
patch applied to the source is live in `$WINEPREFIX/drive_c/windows/system32/` too - restoring only
the source leaves the probe in place for every later run. `cmp` both against the backup afterwards.

**Restore in the *same command* that reads the result.** The rule above is old and I broke it
anyway: I read a probe's output, pivoted to a different tool, and left a `ud2` live in
`Solitaire.exe`. The next two runs came back with 2 critical errors and no login - and I briefly
blamed the profiler I had just started, because the regression and the new tool arrived together.
The giveaway was in the crash text all along: `Unhandled Exception At 0x0000000140306eff --
Illegal instruction`, an address I had patched myself. **A leftover probe looks exactly like a
regression in whatever you did next.** Put the `restore` + `cmp` in the same command as the run,
never in a follow-up, and if a run suddenly gets worse, `cmp` the binary before theorising.


To answer "does this basic block execute", overwrite its first two bytes with `0f 0b`. A hit shows
up as `err:seh:NtRaiseException Unhandled exception code 1007`. The *address* in that line is the
Wine-side rethrow and is the same for every probe - only the presence of the line means anything.

**Always run a control probe** (a `ud2` somewhere that must execute) before trusting a negative.
Three negatives in a row usually means the patched file is not the file being loaded.

### Reading a private interface

For interfaces that are in no `.winmd` and unnamed in `HKCR\Interface`:

1. `HKCR\Interface\{iid}` in the Windows SOFTWARE hive (`./fasthive.py`) often has the
   name even when the metadata does not.
2. Find which DLLs contain the IID bytes. A **proxy/stub** DLL gives the exact method count: find a
   pointer to the IID (that is `CInterfaceStubHeader::piid`), read `DispatchTableCount` at +0x10.
3. Best of all: find the **shipping implementation's vtable** and let its PDB name every slot. For
   a WRL class the GUID bytes sit in a `{GUID, offset}` table under `?s_InterfaceParts@<Class>@...`
   in the `.syms`: that names the class, and its `Partner`/`Private` vtables in the same `.syms`
   name the slots. That is how XAML's `{01dc794b}` turned out to be `IVisualPartner` in minutes. That
   is how `{9d47a108}`'s 13 methods were recovered - the 19-entry `MediaPlayerImpl` vtable in
   `Windows.Media.Playback.MediaPlayer.dll`. Do this before writing a single stub.
4. Failing all that, implement the interface with one traced thunk per slot and let the app tell you
   which methods it calls.

## Mistakes already made - do not repeat them

**Byte-patching a Windows DLL is fine; patching our own code is not.** We have no source for
those binaries, so a byte patch is the only tool there is, and a legitimate state. None is live today; the last
three retired on 09-28. Our own
Wine source is the opposite case: it is vendored in full under `wine/` and there is never a reason
to represent it as a diff.

What *is* true of a DLL patch is that it **cannot be shipped or re-applied as a diff** - that needs
the exact base binary, and Microsoft serves only current revisions. So the thing that matters is
keeping the pristine original (see the byte-patch ground rule) and recording what each patch does
in Findings, not converting it into something else. Re-home one into Wine when the Wine-side fix is
worth having on its own merits, as the CoreMessaging patches were on 09-28 (IOCP/message-pump
behaviour Wine should get right anyway) - but that is an improvement, not a debt. See
Findings-M4.

**There is no "other session", and this file said there was for months.** Every one of the 116
commits in `~/src/wine` is ours, one author, one committer - checked with `git log --format=%an`,
which is all it ever took. The claim sat in the Layout section and the ground rules, was repeated
in `env.sh`, `README.md` and two tool headers, and was **trusted** because AGENTS.md is trusted. It
did direct damage: a list of "files that are not yours" excluded 56 files and ~5,000 lines of our
own work - `combase/roerror.c`, `user32/message.c`, `server/alpc.c` - from being vendored into this
repo, and this same file's toolkit table describes two of those very files as our own
instrumentation. It also produced a bogus explanation for a regression (above). **When this file
asserts a constraint about who owns what or what you may not touch, verify it once and delete it if
it is wrong** - that is the rule at the top, and the "other session" is what it exists to prevent.

**Reason backward from a measurement, not forward from a mechanism.** Several explanations for
Solitaire drawing nothing each fitted every symptom available and were each wrong (Findings-M4).
When a theory "explains everything", that is not evidence - find the measurement that separates it
from the alternatives.

**And keep the measurement separate from the inference drawn from it.** The `ud2` probes showing
Solitaire never calls `Window.put_Content` are sound. The conclusion I hung on them - "so it never
finishes `OnLaunched`" - was not: the app is a *native* engine that renders through composition and
has no reason to call it. A correct measurement will still mislead if the model around it is wrong.

**Read the app's own strings before theorising.** `Solitaire.exe`'s `.rdata` gave up its entire
startup log vocabulary (`{}::Startup START`, `SolitaireApp::StartupComplete -- ...`) with no
symbols at all, which is how we know it never reaches its own startup. Find a log line you *do*
see, dump the strings around it, and read the sequence.

**An app may write its own log.** Solitaire prints `[  0.002] LiveManagerXsapi2 - ...` lines to
stderr naming its own failures, which is better evidence than anything we can infer from outside.
`grep -aE "^\[ *[0-9]+\.[0-9]+\]" run.log` before theorising.

**A crash reporter makes a crash look like a clean exit.** Shapr3D disappeared ~2 s in with exit
status 0 and not one error line. Sentry had caught the engine thread's `abort()` in-process and its
out-of-process `crashpad_handler.exe` wrote a minidump and called `TerminateProcess(app,
0x40000015)`. A kill from *another* process is invisible to anything hooking the app's own exit
paths. If a package has a `sentry/` or `crashpad_handler.exe`, look for its dump first.

**`grep -l` over binaries gives false negatives.** It said no DLL imported
`DwmpGetColorizationParameters`; `strings` found it in `Windows.UI.Xaml.dll` immediately.

**`tramp.py` cannot hook .NET Native managed code.** Its logger's `OutputDebugStringA` raises an
SEH exception, and mrt100 turns any SEH exception in managed frames into a fail-fast.

**A vtable shorter than the interface is a wild jump, not an error.** `dcomp`'s `window_host_vtbl`
had 8 entries and MrmCoreR calls slot 17; the call read past the array into `.data` and jumped
through a NULL. When an interface is in no `.winmd` and unnamed in `HKCR`, give every slot a traced
thunk up to a generous bound and let the app say which it calls.

**A shim can be registered, installed, and export nothing.** A missing `__declspec(dllexport)` gets
you *"Module ... does not implement DllGetActivationFactory"* while the installer reports success.
`llvm-readobj --coff-exports <dll> | grep -c Name:` after every shim build.

**Never queue an APC onto a thread you do not own.** Wine parks its own workers
(`wine_rpcrt4_server`, `wine_threadpool_*`, `wined3d_cs`) in *alertable* waits, so an APC queued
onto one runs inside that wait. `wineuwp` broadcast startup APCs to every thread in the process;
on the RPC server thread the APC reached `CoUnmarshalInterface`, which makes an outgoing RPC call
that *that same thread* has to service. It never returned to its loop, never set
`server_ready_event`, and every later `RoGetAgileReference` blocked - one Media Player run in three
started and never painted. `open_app_thread()` in `dlls/wineuwp/main.c` filters them out by
`GetThreadDescription`; use it for any new broadcast.

**A stack that looks alarming may be the normal state - check it against a working app.**
Solitaire's main thread sits in
`FrameworkApplicationFactory::Start -> RunInActivationMode -> CoreApplicationFactory::Run ->
CoreApplication::RunInternal -> TearDown -> WaitForExit`, which reads exactly like an app that has
skipped its main loop and is shutting down. I reported that as the root cause. **Media Player's
main thread has the identical chain** - it is simply where `CoreApplication::Run` parks while the
real work happens on other threads. The difference between the two apps was in the *other* threads:
Media Player has engine/app workers and a UI thread inside `dcomp`; Solitaire has neither.

**When a thread will not answer, go and look at it.** There is no usable debugger here, but a
watchdog thread inside the process can `SuspendThread` any target, `GetThreadContext` it, and walk
frames with `RtlLookupFunctionEntry` + `RtlVirtualUnwind`, then resolve against `/proc/<pid>/maps`
from the same process. That ended a hunt that three rounds of reasoning had got wrong. **Do not
scan the stack for module-range values instead** - that yields a plausible chain that resolves to
vtables and GUIDs, not frames.

**Instrumentation that alters control flow is an experiment, not a measurement.** A 10 s timeout
added to a wait "just to see" made 12/12 runs pass and looked exactly like a fix. Restoring the
`INFINITE` wait and keeping only the logging reproduced the bug in 3 runs.

**A poll that reports nothing is not a wait, and a poll is not a wake.** Our `CoMsgWaitInProcessEvents`
turned INFINITE into a 16 ms poll for months, and it hid the real bug as latency. CoreMessaging looks at its queues
only when its own wake handle is reported signalled. So a `WAIT_TIMEOUT` from the poll left any item whose wake had
been missed pending for good: a blank Solitaire, a Shapr3D import that never navigated back, a half-loaded panel. The
real fix was win32u waking the message wait on the thread's CoreMessaging completion port, and the poll is gone. The
index for "messages available" still has to be translated back past any handles we append.

**A private export's contract is in its caller's fallback.** CoreMessaging checks `ApiSetQueryApiSetPresence` and,
without `ScheduleCoreMessagingDispatch`, does the job itself with `SendMessageCallbackW`/`PostMessageW`/`SetTimer`. That
code is the specification, down to a return value read as a tri-state. Our export posted `WM_NULL` for months, and
then got the priorities wrong twice (a fail-fast, then a 35,000/s ping-pong). Disassemble the fallback before
implementing the export.

**Wine block-buffers its debug output inside its own process**, so a log line can appear seconds
after the process that wrote it has exited - `stdbuf` on `run.sh` does not help. A poller that must
act while the process is still alive needs a signal from outside the log. For a UWP app the
reliable one is the window's unique colour count; the window's *existence* is not, because it is
created before the app paints and a wedged run has one.

**A successful QueryInterface for a private IID proves nothing in `dlls/dcomp/composition.c`.**
Every object there ends its QI with `else *out = &impl->vtbl; return S_OK;` - a catch-all that
answers *any* IID. `comp_shape_from_any` believed it, so `TryGetAnimationController` on a non-shape
read `bindings[8]` at `+0x290` and `binding_count` at `+0x510` out of a smaller allocation and
faulted about one run in seven. "It is not a `dcomp_visual`, so it must be a `struct visual`" is the
same error in the other direction. **Check the vtbl** - `dcv_from_any`, `visual_from_interop`,
`comp_shape_from_any` and `path_geometry_from_any` all do now.

**A garbage register can look like an empty out-pointer, and one did.** `is_out_pointer()` accepted
any writable word holding zero. After a `WaitOnAddress` wake `rdx` still points at ntdll's futex bucket
lock, which is zero when unlocked, so a dcomp recording object was written into it. That bucket
guards the process heap, and every thread then spun on it. It now refuses Wine builtin images. Any
new "is this an out-pointer" guess must reject static data too, not just unmapped and non-zero words.

**dcomp's painter holds no references to what it paints.** It walks visuals, brushes and surfaces
by raw pointer, while XAML releases and resizes them on its own thread. So anything that frees
paint-visible state must defer to the painter: cache DIBs through `retire_bitmap()`, surfaces
through `defer_surface_destroy()`. Only the painter frees them, under `present_cs`, after the frame.
Freeing directly is what killed Shapr3D when leaving the editor.

**Never edit the user's projects in a test, and never end a test with an unconditional Ctrl+Z.** A test
drag that failed made the script's Ctrl+Z undo the previous real step of the user's drone project
(`Face Offset 20`), and the project syncs to the cloud. Make test edits in a project of our own, check the
edit happened before undoing anything, and prefer orbiting (no edit) when all you need is load.

**Scripted drags are not a real mouse.** `xdotool` moves every 30-100 ms; a real mouse reports every few
ms, and Shapr3D starts a preview only on a move that arrives while it is idle. My sparse drags made the
drone's extrude look like 150 ms per preview; continuous 8 ms moves give ~78 ms. Measure interaction
rates with continuous input (`/tmp/smoothdrag.sh` in the session: a move every ~8 ms), and use the
`dcompinput`/`ptrdisp` trace channels to see input arrive.

**`trace+tracelogging` changes what .NET apps do.** Our ntdll enables every ETW provider for it, so .NET's own EventSources switch on: the Microsoft Store's dependency-injection EventSource then reflected over a delegate .NET Native had not kept, and start-up died with `MissingRuntimeArtifactException`. A crash that appears only under that channel is the channel's.

**.NET Native's first-chance exceptions are visible.** A breakpoint on `mrt100_app.dll`'s `RhpThrowEx` (`WINE_UWP_BP=mrt100_app.dll+45be0@stk:10` for the Store's runtime) logs every managed throw with its caller chain; group them by the exception object (`rcx`) and the first throw of each object is its origin. The app's own string literals (frozen `String` objects in `.rdata`, referenced by `leaq`) name its functions: `/tmp/nstr.py` in the 10-19 session lists them per function.

**Do not leave `WINEDEBUG=warn+heap` on in a session the user works in.** It turns off the LFH and
fills and checks every free: Shapr3D's modelling took 2.3x the CPU, and the user reported extrusion as
"painfully slow" in exactly such a session.

**Frame pacing follows the display, which is 175 Hz on the user's machine.** A fixed 15 ms paint interval
capped the viewport at ~66 frames a second there, and with no back-pressure the app rendered 175 frames
and 70 were shown. `paint_interval()` is one refresh; `present_notify_Presented` makes a present wait
while the previous frame is still unread or unpainted. Test pacing changes at 175 Hz as well as 60.

**Painting must not run on the UI thread per swap chain frame.** At the user's 3440x1366 a full-window
paint is ~6 ms; the swap chain presents 60 times a second, and while those paints ran in the UI thread's
`WM_PAINT` Shapr3D's UI thread had no time left for the app (a 2 s extrude drag: 1.95 s of UI-thread
CPU, nearly all of it `raster_*`/`BitBlt`). The readback job and the repaint timer paint on their own
threads. That walk now runs beside the app's threads, so **every `free`/`realloc` in `composition.c`
waits out a walk in progress** (`guarded_free`, macro'd over `free`); a free that holds a lock the walk
also takes (`dcomp_visual_cs`) deadlocks, so release outside those. Plain `(free)()`/`(realloc)()` only
for lists read under their own lock.

**Tree changes land live here; on Windows they land at Commit.** Shapr3D has two XAML UI threads, and
one's commit painted while the other was halfway through rebuilding a menu's subtree - the menu
vanished for a frame. Paints other than a commit's own now wait for an open batch (`xaml_batches` in
`dcomp/composition.c`, at most 50 ms). Anything new that paints outside a commit has to go through
`present_desktop_hwnd` to get that, and **anything of ours that bumps the scene from a UI thread must
use `scene_dirty()`, not `scene_changed()`**: the latter opens a batch, and a repaint that opens one
waits for itself (Media Player started blank white 3 times in 16 before that was found).

**A heap address seen twice in a trace can be two objects.** A key-frame animation's address carried a
RotateAngle turn and, two seconds later, an Opacity fade with a fresh key count, and I wrote here that XAML
refills running animation objects. The key count can only grow, so it was a new object in freed memory.
Before hanging a theory on "the same object", check for something only one object could show.

**An animation bound to a property replaces its static value; it does not compose with it.** XAML set a
chevron glyph's static `TransformMatrix` to a turn and bound the same turn through an expression; applying
both drew 180 degrees about the wrong pivot and clipped the glyph away. When a value has both, the
animated one wins while it runs.

**A visual's opacity applies to its whole subtree.** The painter used it only to skip nearly transparent
visuals and for a visual's own fill, so every fade of a container (page transitions, toolbars appearing) drew
its children at full strength. `paint_opacity` in `dcomp/composition.c` carries it down; anything new that
paints must go through `paint_emit` to get it.

**The batch gate's timeout must outlast XAML's slowest frame at the user's size.** At 3440 wide a frame that
rebuilds a toolbar takes 70 ms and more; the gate's old 50 ms let a paint draw the new buttons at the origin
with their positions unset. Reproduce layout glitches at 3440 and 175 Hz before calling them fixed.

**A painted frame is not a frame on screen.** dcomp paints into the window surface from pool threads; win32u copies
a surface out only when a message loop goes idle, or every 50 ms. While the UI thread was busy the viewport went
out in 50 ms steps while every paint-side measurement looked perfect. dcomp now flushes after each frame
(`NtUserCallHwnd_FlushWindowSurface`). Measure what reaches the display: `trace+win` `Flushing hwnd` lines, or
`xwd` frames with gaps between them.

**Anything that holds paints back holds the app's swap chain back too, unless it says otherwise.** The XAML batch
gate deferred whole paints, and a swap chain frame reaches the screen only through one, so while Shapr3D's editor was
being built the viewport stood still for up to 250 ms. `paint_swapchains_only` now shows swap chain frames against
the last complete paint list while gated. The same trap had a second layer: `request_repaint` kept one pending timer
and dropped earlier requests. Measure frames *shown* (`dcompgate` `swap chain frame N shown`), not presented or painted.

**A retry per frame is a cost per frame.** dcomp re-read any surface whose texture the caller wrote itself on every
frame, 120 times, until it had pixels. XAML's A8 nine-grid masks are mostly empty by design, so Shapr3D's editor spent
2-4.7 ms of every frame for its first seconds on 4000-5000 single read-backs a second. `dcompperf`'s `surface
readback ... single: N, M upload retries` shows it; retries now back off. Before adding anything that repeats until
something appears, bound it in time and in cost.

**A hover test with a perfectly still mouse proves nothing.** A real hand always moves the pointer a pixel or two. I called the History `...` fixed after a still-mouse test, and the user saw it vanish on the first move. After any click that should keep hover state, `xdotool mousemove_relative 1 0`, then capture. `/tmp/jitter.sh` in the 09-29 session does exactly this.

**After any interaction, check that XAML itself still repaints, not just the picture.** Shapr3D's viewport is the app's
own swap chain and keeps moving when XAML's frame loop has died; a drag-and-drop fix looked perfect on the model while
every menu and tab had frozen. Click a tab or open a menu afterwards, or count `dcompgate` `commit` lines. One failed
XAML tick (an `E_NOTIMPL` inside `OnTick`) stops the loop for good.

**A D2D device context costs six shader compilations - never make one per surface.** Wine's d2d1 built its shape shaders
for every context and dcomp made a context for every drawing surface, even ones XAML fills through the texture: ~1200
compilations per menu open, and the menu's first frame waited for all of them in the command stream. The wait showed up
in dcomp's readback `Map`, which is only the first thing to wait; profile `wined3d_cs` before blaming the waiter.

**A D3D device here is a whole Vulkan device, tens of milliseconds, and apps treat it as cheap.** Win2D creates
one per thumbnail and drops it; each went through a new DXGI factory, wined3d and Vulkan instance. DXGI factories
now share one wined3d and wined3d reuses the last released `VkDevice` (72 ms -> 3.7 ms). When a render thread
stalls, look for device churn with `trace+d3dperf` before anything subtler.

**A tight `xwd` capture loop can freeze what it is measuring.** Back-to-back `xwd -id` grabs at 1280 stalled
our painting: the view cube popup looked stuck on its header for 2 s, while a sampler with 70 ms gaps saw it
finish in 0.9 s. Put a gap between grabs (`GAP=0.04` for `/tmp/winframes.sh`) before calling anything stuck.

**A transform parent that is also an ancestor changes nothing.** XAML's transition wrappers point
`SetTransformParent2` at an ancestor (a popup's scroll content); taking that parent's space dropped the layout
offsets in between. Only a transform parent outside the visual's ancestry moves it (popups, ComboBox dropdowns).

**Know whether a size is painted or local before scaling it.** `paint_public_visual` and
`paint_dcomp_visual` pass the visual's *painted* size to the clip code; scaling it again shrank
Calculator's hamburger to its top bar. Only the regression suite caught it - run it after any painter change.

**An `IDCompositionVisual` slot we answer with a no-op can move things, not just decorate them.**
`SetTransformParent` (slot 9, and `SetTransformParent2` at slot 50) was `dcv_ok`. XAML uses it to
place a popup's visual in the space of the element it is attached to. Ignoring it drew every
ComboBox dropdown relative to the window origin instead: a Settings dropdown opened at (382,9)
instead of over its box at (836,338). Before calling a slot "harmless", name it from the shipping
vtable: Windows' `dcomp.dll` + `dcomp.pdb`, `InteropVisual::Api`, found by searching `.rdata` for
the PDB-named `SetVisible`.

**The catch-all is also how a private interface's call turns into garbage state.** XAML's
`CPopup::ApplyHostBackdropTreatment` queries every popup's visual for `IVisualPartner` `{01dc794b}`
and calls slot 6, `SetWindowBackgroundTreatment(brush)`, without checking the QI. The catch-all
answered with the IDCompositionVisual vtable, whose slot 6 is `SetOffsetY(float)`, so every menu was
built on time and painted ~1e29 pixels down until its open transition ended: the "270 ms menu lag".
When something is in the tree but invisible, look for an absurd number before a missing call.

**The same catch-all hands out `IClosable` as the primary vtable, and `Close()` is slot 6.** On a
typed object with a getter in slot 6, that getter writes through Close's missing argument. It
happened with the radial gradient brush: WinUI closed it, `get_EllipseCenter` wrote 0.5,0.5
through rdx, and the editor crashed on every open. Any new typed object needs a real IClosable
view (`dcv_closable_vtbl`, `visual_closable_vtbl` and `gradient_closable_vtbl` are the patterns).
To find which IID a caller got, log the IID and `__builtin_return_address(0)` in that object's QI.

**A fixed RVA into a Microsoft DLL is a bug waiting for the next Windows build.** ntdll and `wineuwp` knew twinapi and XAML
by 26100.8972 addresses, and a prefix from the ISO's 26100.5074 files came up blank in every app. The only clue was that
wineuwp's window factory was never called. A leftover write into XAML's `HotPatchSpareGlobals` would have corrupted
other builds' data. Look addresses up by symbol (`uwp_symbol`, `resolve_symbols.py`). The same run found a latent bug
in our own code that only an older XAML exercised, so **testing against a second Windows build finds bugs in us, not
just in it.**

**`addr2mod.py` cannot see Wine builtins, and will misattribute confidently.** `+loaddll` traces
only `native` modules (a full Media Player run: 23 `Loaded ... : native`, zero `builtin`), and
`addr2mod` returns the highest base below the target - so every address above the last native DLL
is blamed on it. It reported `mrt100_app.dll+0x1f8440` for a fault in `ucrtbase`; that module's
`SizeOfImage` is `0x86000`. **Check the offset against `SizeOfImage`**, and get the real map from
`/proc/<pid>/maps` (Wine builtins load at fixed addresses, so any run of the same build will do).

**A captured module map is only valid for the build that produced it.** Rebuilding one DLL shifts
every base. A stale map made a correct fix look like a failure - the fault had moved to a new
address in the same function, and the old map pointed at the wrong DLL entirely. Re-capture after
every rebuild.

**To find who called a hot libc function with a bad pointer**, put a temporary probe in it that
`VirtualQuery`s the arguments and logs `__builtin_return_address(0)`. `dlls/msvcrt/wcs.c`'s
`_wcsicmp_l` named `dcomp!object4_view_TryGetAnimationController` on the first hit. Revert it
afterwards and `cmp` against the backup - and note the probe *shifts the module layout*, so any map
captured before it is stale.

**A stubbed `GetRuntimeClassName` on a WinRT object that .NET receives back is a silent drop.**
Wine's `Windows.Foundation.Uri` returned E_NOTIMPL there, and Shapr3D's `Uri -> BitmapImage`
converter discarded every `ms-appdata` thumbnail with no error anywhere. Any object that crosses
into managed code needs a real class name and trust level.

**An empty collection is a refusal in disguise.** `MediaPlaybackItem.AudioTracks` handing back an
anonymous empty `IVectorView` made the app's cast to `MediaPlaybackAudioTrackList` throw, so it
dropped the item before adding it to the playlist - and every subsequent symptom appeared in the
*player*, one layer below where the damage was done. The collection has to be the right class, and
what makes it the right class is `GetRuntimeClassName`: .NET types the RCW from that string, so
naming the interface instead of the class throws `Unable to cast object of type 'EETypeRva:...'`.

**A generated parameterised IID is only right if the IDL agrees with the shipping metadata.**
`IID_IVectorView_AudioTrack` from widl does not match what the app asks for, because Wine's
`windows.media.core.idl` gives `AudioTrack` the default interface `IAudioTrack` and the metadata
gives it `IMediaTrack`. `IID_IVectorView_MediaPlaybackItem` *does* match. Check with `piid.py`
before using a generated one.

**A `virtual` destructor in a C++ COM class takes a vtable slot.** Declared before the methods, it becomes slot 3 and
shifts every method after it: a hand-written `IWeakReferenceSource` returned a garbage HRESULT from "`GetWeakReference`".
Keep destructors of COM implementations non-virtual (the C++ shims' `WeakRef` was one).

**An untyped traced thunk must not declare more parameters than its callers pass.** `winegameplatform`'s generic
slot thunks were declared with six arguments and called `generic_slot` with five; gcc made that a sibling call and
stored the fifth argument in the incoming stack-argument area, which the declaration says is the callee's but which
the Xbox app (passing three) had in its own frame. The app then released a clobbered factory pointer and faulted
right after `add_ApiStatusChanged`. Declare only the register arguments (four, `this` included) for a thunk that
stands for slots of unknown arity, and check the disassembly for a `jmp` that writes `[rsp+0x28]` first.

**`InitOnceBeginInitialize` needs a real `pending`.** Passing NULL faults inside kernelbase (a write to address 0 at
`InitOnceBeginInitialize`), and ignoring it would re-run the initialisation on every call: `BOOL pending;` and
`if (InitOnceBeginInitialize( &once, 0, &pending, NULL ) && pending)`. winexboxauth's first run died on its first
factory this way, before any trace of its own; `+loaddll` gave the module bases that put the fault in kernelbase.

**`InitializeCriticalSection` in Wine leaves `DebugInfo` as `(void *)-1`.** Naming a heap critical section with
`cs.DebugInfo->Spare[0] = ...` (the pattern for *static* ones, which carry their own `CRITICAL_SECTION_DEBUG`) writes to
`0x27`: the first `xgameruntime` port crashed Halo in `XUserAddAsync` that way. Use an SRW lock for per-object state.

**A DLL beside a game's exe beats Wine's builtin of the same name.** The old `xgameruntime.dll` stand-in had been
copied into Halo's stage, `game/` and `subgames/CampaignS1/`, and those copies load before `system32`'s builtin
(`build-probes/` had one beside `probe-gdkchunks.exe` too). Check `+loaddll` says `builtin` before testing a port.

**Wine does not show a child window that belongs to another process.** A cross-process `WS_CHILD` (WebView2's Chromium
window parented to the app) paints nothing on screen (`probes/probe-xchild.cpp`); Wine's `dlls/webview2host` (installed as the runtime's EmbeddedBrowserWebView.dll) shows the browser
through `CapturePreview` frames in a popup instead.

**A process started from a hosted app inherits its whole environment.** The Xbox app's package identity, DLL overrides,
`WINEPATH` and `WINE_UWP_BP` all reached the game it launched. Launch other packages with a clean environment block
(`clean_environment()` in `dlls/winegameplatform/install.c` and `dlls/windows.system.hostlauncher`).

**In a hosted app, `CoreApplication.MainView` can be a view with no window.** The activation stands up its own view and
the CoreWindow lives there; combase's `get_MainView` wrapper answers the view that has the window (Findings-M4
2026-10-15 night). A BP on twinapi's `CoreApplicationView::get_CoreWindow` (`uwp_symbol` it) shows which view is which.

**A WinRT delegate is not IInspectable.** `TypedEventHandler<A,B>` is three `IUnknown` slots and
`Invoke` - slot **3**, not 6. Calling slot 6 on one lands past the end of its vtable.

**Null is not a neutral answer.** This cost three separate debugging sessions:
- `{9d47a108}` slot 6 looked like a getter. It is `add_CastingRenderLocationChanged`, an *event
  registration* - writing NULL through its first argument overwrote the caller's handler and killed
  the process.
- `MediaBreakManager::get_PlaybackSession` returning NULL destabilised the whole run; the app reads
  the session straight back out and uses it. Handing back the player's real session made Media
  Player render.
- The composition default slot wrote a fake object pointer into whatever looked like an
  out-parameter. For `IKeyFrameAnimation` those are `HSTRING *`, `TimeSpan *` and `UINT32 *`; .NET
  Native marshalled the bogus HSTRING and `memmove`d from the garbage it decoded.

  **Before answering a getter, find out what the caller does with the answer** - from the shipping
  implementation, or by instrumenting the call.

**`E_NOTIMPL` from a shim can stop an app with every thread idle and no stack pointing at it.**
Solitaire's toast shim answered `AddToSchedule` and `GetScheduledToastNotifications` with
`E_NOTIMPL`. The caller turns that into a throw on a path that never completes its bootflow item,
so the whole boot sequence stopped - and `WINE_UWP_DUMP_STACKS` showed *nothing* wrong: workers
parked on their task queues, UI threads pumping, no spinning. **A lost continuation looks like an
idle process, not a hung one.** When every thread is idle and the app has stopped making progress,
look for the last refusal, not for a blocked stack. Accepting and dropping (`S_OK`) is the right
answer for a delivery API with nowhere to deliver to.

**A field that looks like runtime state may be file data.** `ScreenFile+0x14c = 0` on the popup
looked like "load never finished" for a whole stretch. It is byte 0 of the `.gui` file's header -
the popup is non-opaque by design. When a structure has a parsed-from-disk origin, extract the file
(Solitaire's archives are zips of Brotli-compressed chunk files) and compare bytes before hunting
for the code that "should have set" a field.

**Two vtables found from their RTTI are not aligned with each other.** I probed "slot 28" of
`StartupSplashScreen` and of `GenericDialogScreen` and concluded the splash's `OnLoaded` runs and
the dialog's does not. The splash **does not override `OnLoaded` at all**, and its vtable is
shorter, so that address belonged to the next vtable in memory - a different interface's method
entirely. `rtti.py` gives you the vtable; print both side by side and compare *addresses*, and
check the class actually overrides the method (`grep` for `OnLoaded@<Class>@Sgi@@`) before reading
anything into a slot index.

**A stack frame whose function contains a familiar log string is not necessarily doing that work.**
`whatfn.py` put "Inventory - Failed to Find/Load asset" in the function a parked thread was in,
which read exactly like the deadlock. It was an inlined worker loop parked on an empty MPMC queue -
a normal idle worker. Read the *code* at the frame (here, a ring-buffer dequeue then
`WaitOnAddress(..., INFINITE)`) before believing the string.

**A trampoline must not keep the saved `rsp` in `rax` across the call - and this one bug has now
produced three wrong conclusions.** `rax` is the return-value register, so the callee overwrites
it; `mov rsp, rax` on the way out then sets `rsp` to garbage, the register pops restore rubbish,
and the app faults on the **first instruction after the hook**. The crash therefore points at the
*hooked* code, not at the probe - Solitaire reported
`Unhandled Exception At 0x0000000140518ccd -- Access violation writing location 0x8`, which is the
instruction the trampoline returns to. Use `rbx` (callee-saved) to hold the pre-alignment `rsp`,
and save/restore it with the volatiles. `tramp.py` does this now and calls the app's own logger
successfully.

This is what was behind all of:
- "the app's own logger cannot be called from a trampoline" - written into this file, and false.
- "`OutputDebugStringA` raises and the unwinder derails on a cave with no `.pdata`" - also false,
  though `tramp.py` adds a real `RUNTIME_FUNCTION` for the cave now anyway.
- a previous session's "`UWPTFUT` never fires, so the download task is never created".

**Bisect a probe before blaming the code**, with the three modes `tramp.py` has: `--nolog`
(displace and jump only), `--noop` (full register save/restore, no call), and the call itself.
`--noop` working while any call crashed is what localised the bug to the call sequence; a call to
`xor al,al; ret` crashing is what proved it was not the callee.

**A refusal is usually the fatal thing** - `E_NOTIMPL` from an event registration, a `@ stub` in a
.spec, an unresolvable delay-load, an interface revision answered `E_NOINTERFACE`. Registering an
event has to succeed even when the event never fires.

**But do not claim an interface you cannot implement.** Answering XAML's
`IReferenceTrackerInternal` made it call tracking methods on an object that could not honour them:
51 fail-fasts. A plain WinRT object is *supposed* to return `E_NOINTERFACE` there. Both
`E_NOINTERFACE` and `E_NOTIMPL` are answers callers handle; an invented value is not.

**`E_NOINTERFACE` can be the fatal answer, and this is the exception to the rule above.**
"Do not claim an interface you cannot implement" is about interfaces with a behavioural contract -
XAML's `IReferenceTrackerInternal` calls tracking methods the object then has to honour. It does
**not** apply to a property bag. Wwise asks for `ISpatialAudioDeviceConfigurationStatics` with
C++/WinRT's `.as<>()`, which *throws* on `E_NOINTERFACE`; on its own audio thread, with nothing to
catch it, that killed Solitaire the moment it reached its splash screen. Refusing was not the safe
answer, it was the lethal one - the caller is written knowing the platform always has that class.
Before refusing an interface, ask whether it has a contract you would be breaking, or whether it is
just properties you can answer honestly ("this machine has no spatial audio").

**Wine's d2d has no layers and no geometry combining.** `PushLayer` is a FIXME stub and
`CombineWithGeometry` returns `E_NOTIMPL`, so anything clipped through them draws unclipped. `svg.c`
clips by drawing into an offscreen bitmap target and filling the clip geometry with a bitmap brush
of it. Also: an `ID2D1GeometryGroup`'s fill mode **overrides its members'**, so an evenodd hole
inside a WINDING group is filled solid. Test SVG changes with `probe-svg.exe`, which draws through a
bitmap target the way Win2D does, not straight into its WIC target.

**SVG smoothing alone misses direct Win2D geometries.** Xbox's Game Pass marks use `FillGeometry`
on larger surfaces, then shrink during composition; Premium uses `DrawSvgDocument` at its final size.
Antialias both geometry entry points and SVG documents, then filter the complete source footprint
when reducing the composition surface. A single bilinear reduction samples only four texels and can
still drop glyph strokes. `probe-svg.exe Z:\tmp 64 --geometry` checks direct fill/stroke coverage,
explicit aliasing, transform/target restoration, clips and DPI; `python3 probes/check-area-sampler.py`
checks the composition minifier. Findings-M4 2026-10-16 (Game Pass).

**Two paths feed a CoreIndependentInputSource, and they must not both fire.** win32u's
`forward_pointer_to_independent_input` copies *every* pointer message on the CoreWindow to it
(Solitaire needs that); dcomp's input-sink hook routes only the input over a swapchain (Shapr3D
needs that). Both at once handed Shapr3D every toolbar click as a viewport press - which also made
Sketch "pick a plane" by itself. dcomp sets `__wine_dcomp_input_sink` on its sink and the forwarder
skips it. When input arrives that nothing you wrote posted, `WINEDEBUG=trace+msg` and grep `forwarding`.

**`make dlls/dcomp` in the build tree does nothing** - it names the directory, which is up to date.
Build one DLL with `make -j16 dlls/dcomp/all`, and check the DLL's timestamp before trusting a run:
three runs this session tested a build that had never been made.

**We build one DLL at a time, so the rest of the build tree goes stale, and a full `make` is a different Wine.**
The first `make install` for packaging on 09-29 rebuilt ~500 modules. It also turned up three build breaks nobody had
seen: our `rpcrt4/cpsf.c` no longer compiled, `wow64win` lacked a thunk for our `NtQueryCompositionInputSinkLuid`, and
`icu.dll` failed to link. The icu failure came from `libs/c++` and `libs/icu*` archives mixing objects from an older
compiler setup (MSVC-mangled names) with new mingw gcc ones. `rm -rf libs/<lib>/x86_64-windows`, recreate the object
directories (`grep -oE '^libs/[^/]+/x86_64-windows/[^:]*\.o' Makefile | xargs -n1 dirname | sort -u | xargs mkdir -p`)
and rebuild. After a full rebuild, run the suite and `kb` as for any other change.

**A file dialog opened from a test can land on the user's real desktop.** Portal requests go to whatever portal backend the session bus activates. Even under `dbus-run-session` on `:95`, KDE's portal inherits `XDG_SESSION_TYPE=wayland` and draws on the user's Wayland session, and it keeps running after the test. It happened on 09-29. Pass files as arguments instead. To test the real dialog, also export `QT_QPA_PLATFORM=xcb XDG_SESSION_TYPE=x11`. Afterwards kill that bus's `xdg-desktop-portal*`, `kioworker` and `ksecretd` processes: find them by `DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-*` in `/proc/<pid>/environ`, and never touch the user's `/run/user/1000/bus` ones.

**Anything `run.sh` runs after `cd "$S"` runs in the app's environment.** With the stage directory as the working directory and the frameworks on `WINEPATH`, `reg.exe` exits 5 and writes nothing. `run.sh` discarded its output, so every fresh prefix died on its first launch for weeks (Feedback Hub, and every packaged install), while the dev prefixes had the key from before `WINEPATH` existed. **A prefix that has been through many runs hides first-run bugs: test from an empty data directory.** The same run found `[ -x "$PY" ]`, which is false for a bare `python3`, silently skipping the CLSID registrations outside the venv.

**`tools/make_makefiles` builds `SOURCES` from `git ls-files`.** A new `.c` file that is not yet
`git add`ed is silently dropped from its `Makefile.in` and the link fails with an undefined
reference. `git add` first, then `make_makefiles`, then `./config.status` in the build tree. Check
`git status` afterwards: it also "fixed" a committed Makefile that had never listed
`numberformatting.c`.

**A NULL out-parameter with S_OK across apartments means a wrong IDL direction.** Windows.Graphics
got a NULL `IWICMetadataQueryWriter` from a call that succeeded, because `wincodec.idl` lacked
`[out]` on `IWICBitmapFrameEncode::GetMetadataQueryWriter`. The stub treated it as `[in]`, created
the writer, never sent it back and freed it. `WINEDEBUG=trace+rpc` shows it:
`stub_do_args param[0] ... IsIn` with no `IsOut`.

**To see what XAML actually put in a composition surface, dump it.** Writing
`surface->cache` to `Z:\tmp\*.raw` from `surface_cached_pixels` and converting with
`magick -size WxH -depth 8 bgra:f.raw f.png` ended a "decode is broken" theory in one run. The
pictures were there, and the bug was in how the brush was painted.

**Ignored FailFasts are where the unimplemented APIs are.** `RoFailFastWithErrorContextInternal2
hosted UWP, ignoring FailFast` follows a `dump_stowed_exception` whose `frame[N]` lines are raw
addresses. Resolve the first few against the module's PDB `.syms` (with the base from a `+loaddll`
run) and disassemble just before the return address. XAML reports through a helper at
`windows.ui.xaml+0x2f790`, called with the HRESULT, so the vtable call just before it is the refusing
API. That found both `IDWriteFontFamily2::GetMatchingFonts` and dcomp's missing easing creators in
one pass each.

**Text quality lives in dwrite's glyph run analysis, not in the fonts.** XAML rasterizes text
itself with `CreateGlyphRunAnalysis(NATURAL_SYMMETRIC, grayscale)`, not through D2D's text drawing.
So a D2D `DrawTextLayout` probe can look fine while XAML text does not. `probes/probe-fonts.cpp`
lists the system collection and a family's faces; `probes/probe-text.cpp` renders `Family:weight`
lines through D2D. The fonts are Windows' own: all 348 files of a Windows installation's `Windows\Fonts`, copied
into `$UWP_BIN/win11/Windows/Fonts` and installed into the prefix's `C:\windows\Fonts` by
`install-overrides.sh`. win32u registers them under their bare file names, and dwrite opens them
from there (`trace+dwrite` shows `C:\windows\fonts\SegUIVar.ttf`). The host's fontconfig fonts
(`~/.local/share/fonts`) are still loaded too, but a face present in both comes from the Windows
directory. The Windows partition is `/dev/nvme0n1p2`; mount it read-only at `/mnt/win` to refresh
the copy.

**`.as<>()` vs `try_as<>()` is the whole difference, and you can tell which one threw.** C++/WinRT's
`throw_hresult` is a switch on the HRESULT with one `call` + `int3` per value, so the **return
address in the app's own crash callstack lands in the branch for the specific HRESULT**. For
Solitaire it landed in the `0x80004002` arm - `E_NOINTERFACE` confirmed, not inferred. Disassemble
the innermost app frame and read which `cmpl $0x8000...` it fell through to.

**An app's own crash dump may carry a better stack than anything you can capture.** Solitaire prints
`JSON_CALLSTACK:{...}` with raw addresses. The app's own frames are at a fixed base
(`0x140000000`), so `xref.py`'s `.pdata` lookup names every one of them with **no module map and no
symbols** - and the strings inside each function name the subsystem
(`CAkAudioMgr::BeginPerform`, `AK_MSCoreAudioThread`). That is how the audio thread was identified
after `WINE_UWP_DUMP_STACKS` had failed to find it. `grep -oE 'JSON_CALLSTACK:.*'` first, always.

**Constants that do not ship can be your own, if they are only ever compared with each other.**
`SpatialAudioFormatSubtype`'s strings appear nowhere in this Windows image (a UTF-16 search of every
System32 DLL finds nothing), so there was nothing to copy. Inventing a value and presenting it as
Microsoft's is the mistake this file warns about - but here the app only compares those strings
against each other and against `ActiveSpatialAudioFormat`, which the same shim returns. Distinct and
stable within the process is sufficient; say so in the shim, and say what would break the reasoning
(anything passing one of them to a real audio API).

**A `QueryService` that answers every service is a catch-all QI one level up.** `wineuwp`'s window
factory returned itself for any service GUID. When the share UI asked for an
`IApplicationInvocableFlow`, twinapi.appcore took the factory for one and faulted. Answer only
the services you know, and `E_NOINTERFACE` otherwise.

**`out T[]` in a `.winmd` signature can be a FillArray, not a ReceiveArray.** `winmd.py --sig` prints both as `out u1[]`; when a size parameter follows (`GetData(out u1[] buffer, u8 bufferSize)`), the caller passes `(UINT32 length, BYTE *buffer)` and the callee fills it. Treating it as `(UINT32 *, BYTE **)` wrote the data size through the length and crashed the Xbox app at 0x22C9.

**Never guess which argument is the out-parameter.** In the StateRepository interfaces it is always
the *last*; in `get_DescriptionReference(HSTRING **)` it is the *first*. Get arity from metadata
(`gen-staterepository.py` generates a table from the vtables plus `llvm-undname`), or you will write
eight bytes through the wrong argument and leave the real one holding the caller's stack garbage.

**Measure a working app before theorising about a broken one.** "Media Player has two
`CoreApplicationView`s" looked like the bug for two rounds of work. Calculator has two as well, and
works. One census would have saved both.

**Two data points that a third reading explains equally are not a conclusion.** An early finding
about XAML's provider-kind dispatch was inferred from two byte-patch experiments and was wrong; the
type resolved fine and the real failure was three layers further on.

**Absence of a log line is only evidence if the line would have been printed.** A run looked fixed
because `grep "Call failed"` found nothing - that string only appears when
`WINE_UWP_TRACE_ORIGINATE` is set, and that run did not set it.

**A never-completing async is worse than a failure, and it is the commonest UWP-under-Wine trap.**
Three of Solitaire's blockers were the same shape: a Windows class whose implementation needs an
out-of-process broker that does not exist here (`TaskbarManager` -> ImmersiveShell,
`StoreContext` -> ContainerHostActivation, `WebAuthenticationCoreManager` -> the token broker). It
does not fail the `IAsyncOperation` - it simply never completes it, so the app's `.get()` parks
forever with no error to handle and no timeout. A stand-in must hand back an operation that is
**already in the Completed state** *and* whose `put_Completed` invokes the handler immediately, so
both `.get()` (which checks `IAsyncInfo::Status` and skips the wait) and `co_await` (which
registers a handler) work. `shims/taskbarmanager` is the reference implementation.

**The last line in the log is not the blocker.** It is only the last thing that *logs*. This worked
three times running on Solitaire and then failed twice: the Xbox auth path I "identified" that way
had in fact completed, and `UserActivityChannel` fails with a clean HRESULT the app handles. Confirm
with a probe (does the caller's return point execute?) before writing a shim for whatever appears
last.

**"Stuck" is not the same as "blocked", and a sampling profile tells them apart.** Solitaire's
bootstrap thread looked blocked for a whole session - no log output for five minutes, a continuation
that never fired, a future never fulfilled. `WINE_UWP_PROFILE` showed it is **busy**: it spins in
`WinUwp::BootStrap::Startup`, calling `this->vtbl[5]()` (a tick), which pumps a task queue and
returns as soon as any of three flags at `+0x208/+0x20a/+0x210` is set - "finished", "failed",
"cancelled". None ever is. That reframes the question from "what is it waiting on" to "what should
have set a flag", and it is not something a single stack snapshot can tell you. Reach for the
profiler before concluding a thread is parked.

**`WINE_UWP_DUMP_STACKS` negatives are not evidence.** It unwinds ~10 frames per thread, and when
`RtlLookupFunctionEntry` fails it guesses `rip = *rsp` and derails. It reported *no thread inside
`AppHost::EngineAppStartup`* while a `ud2` on that function's success path proved one was. A
positive hit from it is good evidence; an absence is not. Two conclusions in one session were
nearly drawn from its negatives.

**Its frame lines come from `dump_one_thread_stack`, not `dump_stacks_thread`.** Grepping for
`dump_stacks_thread STACK` gets the thread *names* and silently drops every frame, which looks
exactly like "the unwinder produced nothing". Use `grep -aoE "dump_(one_thread_stack|stacks_thread) STACK.*"`.

**A shim registration can be silently un-done after it succeeds.** `install-overrides.sh` copies
DLLs into system32; the next `wine` invocation then runs a prefix update, and that update
re-registers Wine's builtin WinRT classes - overwriting the `ActivatableClassId` `DllPath` a shim
block wrote seconds earlier. Every `reg add` reports success and the shim never loads. The tell is a
class you never touched (`Windows.Devices.Geolocation.Geolocator`) being rewritten with the same
timestamp. `install-overrides.sh` now asserts the shim classes again after the prefix settles and
**verifies**, retrying three times. Two traps inside that fix:

- **`wine reg query` is itself a `wine` invocation and can trigger the clobber it is checking for.**
  I destroyed the state I was measuring this way.
- **Reading `system.reg` instead does not see a key written moments ago** - it is still in
  wineserver's memory until a flush - so the first verifier reported a false failure for a shim it
  had just installed correctly. Ask `wine reg query` first, fall back to the file, and loop.

**And the retry loop itself had a bug that made every *new* shim fail its first run.**
`reassert_activatable` declared `local cls="$1" ... key="...\\$cls"` in one statement; in bash the
`$cls` inside that same `local` still expands to the *caller's* `cls`. So each reassert wrote to
whatever class an earlier loop had left there (`ActivatableClassId\PackageUser`), the verifier
kept reading the untouched Z:-path value, and the run logged `shim registration did not stick`.
It "fixed itself" on the next run only because the verify loop sets the caller's `cls` to the
right class before calling. Certificate, FileSavePicker and FolderPicker each lost a run to it.
Never refer to a variable declared in the same `local`/`export` statement.

**Since 10-21 the writes and the check are Wine's `wineuwp-setup`** (`programs/wineuwp-setup`), which reads the live
registry from inside the prefix, so neither trap above applies; `install-overrides.sh` waits with `wineserver -w` and
runs `wineuwp-setup --verify` (exit 1 while a class was wrong, after writing it again) up to four times. Its output is
checked against the shell version's: on two reflinks of `prefix-test`, `system.reg`/`user.reg` identical apart from
device `ContainerId`s, the same files, and the generated `wine-uwp-clsid.reg` / `wine-uwp-single-module.reg` byte
for byte (Findings-M4 2026-10-21). A path built from `GetSystemDirectory` + `\..` is `C:\windows`, not `C:\`: the first
port put the WebView2 runtime in `C:\windows\Program Files (x86)`; only the file-list diff caught it.

**`pkill -x wineserver` kills every prefix's wineserver, not yours.** On 10-15 it took down the user's running Xbox app's server on `:0` while looking for my own test prefix: their `XboxPcAppFT.exe` survived as an orphan with no server. Stop a test's server with `wineserver -k` under that prefix's `WINEPREFIX` (or by its pid, checked against `/proc/<pid>/environ`), never by name.

**Do not force `wineboot -u` to settle the prefix.** It starts a fresh `services.exe`/`explorer.exe`
set on every launch, and those leftovers are exactly what run.sh warns break the activation race.
`wineserver -w` plus a retry loop is enough.

**Dropping a DLL named after an apiset works only if Wine has no schema entry for it.**
`find_apiset_dll()` consults `Peb->ApiSetMap` *before* any file is looked at, and apiset matching
**ignores the trailing version fields**. So a file named `api-ms-win-appmodel-runtime-l1-1-1.dll`
is ignored, because `l1-1-1` matches `apisetschema.spec`'s `...-l1-1-2 = kernelbase.dll`; but
`api-ms-win-appmodel-advertisingid-l1-1-0.dll` *is* picked up, because that apiset is in no entry at
all and the delay-load helper falls back to a runtime `LoadLibrary`. Two apisets, two outcomes.
**Grep `dlls/apisetschema/apisetschema.spec` first.** When the apiset *is* in the schema, repairing the
import thunk is the tidier fix than editing the loader: `dlls/wineuwp/main.c` walks each
module's import tables and replaces the stub, driven by a one-entry table.

**An unresolved import is not a `@ stub`.** `EXCEPTION_WINE_STUB` (**0x80000100**) with
`wine: Unimplemented function <dll>.<func>` can mean the loader's `allocate_stub()` invented a
raising thunk for an import nothing implements - in which case **grepping the Wine tree for the
function name finds nothing**, which is confusing if you assume a `.spec` must declare it.

**Imports you need to patch may be *delay* imports.** They are in a different data directory and
`llvm-readobj --coff-imports` labels them `DelayImport {` - easy to skim past. A patcher that walks
only `IMAGE_DIRECTORY_ENTRY_IMPORT` finds nothing and looks broken.

**`wineuwp.dll` gets a stale copy in the prefix like any other builtin**, and it shadows the build
tree - so a correct fix looks like it did nothing. This is already documented below for Wine DLLs
generally and I walked into it anyway, because I checked the *timestamp* rather than `cmp`.
`wineuwp-setup` (via `install-overrides.sh`) refreshes it and says `refreshed: stale wineuwp.dll in the prefix`.

**A guessed signature for a private export can silently drop work.** `SHTaskPoolQueueTask` had
been implemented as `(flags, proc, context, cancel)` with a comment explaining why a small "proc"
must be ignored - it was the options word. Every async operation of the genuine Windows.Graphics
then never completed, and the app sat idle with no error anywhere. When a native DLL calls a
private export, read a **caller's** disassembly for the argument count, and find the callee's
signature in its PDB (Windows' `shcore.pdb` names `WorkThreadManager::s_QueuePoolTask`'s
parameters). With no RTTI, find a class's vtable by searching `.rdata` for a pointer to one of
its PDB-named methods (the deleting destructor is a good one) and read the neighbours.

**Give every slot a traced thunk before implementing anything.** `IStoreContext` has 26+ methods;
a traced stand-in returning `E_NOTIMPL` (with event registrations answered `S_OK`) showed the app
calls **`GetDefault` and nothing else**. Merely being able to *create* the object was the entire
fix. Guessing which methods mattered would have been days of wasted work.

**Read a suspicious ternary twice before "fixing" it.** `RtlGetDeviceFamilyInfoEnum` has one that
reads inverted; it is correct, because `get_package_env` returns an `NTSTATUS` and 0 means success.

**Do not add fields to a struct whose prefix is shared.** Everything in `dlls/dcomp/composition.c`
starts with `struct comp_object`'s three fields and reuses its AddRef/Release. A field added there
landed on `kf_animation`'s `target[]` and made Release iterate 0x664f garbage entries - the fault
report showed `rdx 007300660066004f`, `"Offs"` from `"Offset"`. Put new state in its own allocation.

**Lock-timeout lines are noise at start-up and a verdict later on.** This file calls the
`loader_section` retries "normal noise", and for the start-up burst they are. But a `peb_lock`
timeout, or `blocked by <tid> (in DllMain of ..., reason 3)` (a thread *exiting*) after the app is
up, appeared in **every** failed Shapr3D soak run and in no good one. I read past them for a whole
session. `grep -ao "blocked by [0-9a-f]* ([^)]*)"` across all of a soak's logs, split by outcome, is
a one-line discriminator. Then take the named tid's context with `probe-threads.exe`.

**Do not blame the shared tree until you have dated proof, and do not declare an app broken
without a screenshot.** Media Player rendered and navigated, then a later run failed to start. I
wrote both of the following into this file, and both were wrong:

- *"ntdll and kernelbase were rebuilt minutes ago by the other session."* There is no other
  session (see below), and the timestamps were misread too. I had run
  `ls -lt --time-style=+%H:%M`, which prints **no date**, saw `03:43`, and assumed today. With
  `--time-style='+%m-%d %H:%M'` those DLLs are from **09-19**. The only file rebuilt that day was my
  own `windows.media.playback.mediaplayer.dll`. Always print the date; also check
  `$WINEPREFIX/.update-timestamp` for a wineboot and `cmp` the byte-patched prefix DLLs.
- *"Every app including Calculator is dying."* That came from counting log lines
  (`deadlock=4 xaml=2`). Those same counts appear in **healthy** Calculator runs - the
  `loader_section` retry messages are normal noise. A clean run with `import`/`identify` showed
  Calculator byte-identical to baseline at 249 colours. **Judge an app by its pixels**, never by a
  grep tally.

I then got the *third* explanation wrong too, and wrote it here: "msftedit's `DllMain` is
intermittently slow". It is not. The real chain, found by timing the load in a five-line probe:

**`import` grabs the X server, and a hung one wedges the display for every Wine app.** This is
the whole chain, and the first link is our own tooling:

1. ImageMagick's `import` takes an X server grab while it captures. If it hangs - and under a
   `timeout` it will eventually be killed mid-capture - the grab outlives it and `:98` stops
   answering **any** client. `pgrep -x import` finds the straggler; `pkill -x import` makes the
   display answer again *instantly*, with no Wine process running.
2. With the display wedged, Wine's first `RegisterClassW` in a process runs
   `win32u/driver.c:load_desktop_driver`, whose "wait for graphics driver to be ready" is
   `send_message(desktop, WM_NULL)` with **no timeout** - taken while the caller holds the loader
   lock.
3. msftedit's `DllMain` calls `RegisterClassW`. So the log says
   `blocked by <tid> (in DllMain of L"msftedit.dll")` and `first-view startup not taken`, and the
   innocent DLL gets the blame. On a healthy display `LoadLibrary("msftedit.dll")` takes **153 ms**
   (`./probes/probe-load.cpp` reports per-DLL load times).

**`xwd -root` shows a Wine popup over a Wine window as black.** When any window uses a colormap other than the root's
(Wine's do), `xwd` takes its multi-visual path: it merges all windows with the same visual and colormap into one region
and reads that region back from the first such window, the one underneath. Where the popup covers it, that read is
black. The Xbox app's purchase dialog was reported "black on screen" on 10-21 for this reason alone, with the runtime,
the host and the popup all working: a single `XGetImage` of the root had the dialog. Capture the screen with
`probes/xcap.c` (`gcc -O2 -o xcap probes/xcap.c -lX11`, `./xcap > s.ppm`), or the popup itself with `xwd -id`. A
one-colour rectangle exactly at a popup's bounds in an `xwd -root` capture is this trap until a raw capture says otherwise.

**Do not use `import`.** `xwd` is installed now (`xorg-xwd`) and does not grab:
`xwd -id <win> -silent > f.xwd && magick xwd:f.xwd f.png`, or `-root` for the whole screen. If you
must use `import`, reap it after every capture (`timeout 15 import ...; pkill -x import`). When a
run looks impossible, check `DISPLAY=:98 timeout 10 xdpyinfo` *first*; the wedge survives
`wineserver -k` because it is not a Wine problem at all.

`xwd` + `magick` and `import` produce **byte-different PNGs of identical pixels**, so a capture
taken with one cannot be `cmp`-ed against a baseline taken with the other. Compare those with
`magick compare -metric AE a.png b.png null:` - 0 means identical - and note the baseline
`/tmp/calc-regress.png` is a 1600x1000 *root* capture, not a window capture.

Two more traps from the same chase:

- **`WAYLAND_DISPLAY` survives `export DISPLAY=:98`.** Wine prefers `winewayland.drv` when it is
  set, which puts the app on the user's real compositor - which we are told never to do. `run.sh`
  now unsets it; do the same in any wrapper that does not go through `run.sh`.
- **`pkill -f <pattern>` matches your own shell command** and will kill your own session. I did
  this again (`pkill -f ZuneMusic`, exit 144) after writing the warning below. Use `pkill -x` on
  the image name, or `ps -eo pid,args | grep ... | grep -v grep`.

**`pgrep -f "<exe name>"` matches your own shell commands** containing that string, including
background pollers you started earlier. Use `ps -eo pid,args | grep ... | grep -v grep`.

I killed my own shell with this **a fifth time** (exit 144), with `pkill -x soak2.sh` - and the
`[s]oak2.sh` bracket trick in the *same command line* did not save me, because the literal string
`soak2.sh` appeared earlier in that line, in the `pkill` argument itself. The rule that actually
works: **never let the pattern appear literally anywhere in the command line.** Build it from
pieces and exclude your own pid:

And a **sixth time**, in a new variation worth spelling out. I used the split-string trick
(`awk '/impor""t/'`) believing that was enough - but splitting the literal only stops *awk's own
pattern* from matching; the string still appears in the enclosing shell's command line, so
`ps | awk` matched my own shell and `xargs kill` killed it. Worse, the count it printed first
("3 stray processes") was itself just my shell plus its awk, so I acted on a reading that was
entirely my own noise. **Splitting the string is not sufficient on its own - you must also exclude
your own pid.** Both halves of the rule below matter; either alone will bite you. And before
believing a process count, check whether the matches are your own pipeline.

```
P=soak2; ME=$$
ps -eo pid,args --no-headers | awk -v pat="$P"".sh" -v me="$ME" '$1 != me && index($0, pat) {print $1}' | xargs -r kill
```

**A background run loop needs a "process was seen" latch**, or the first poll fires before the app
exists and every run is killed after 8 s. I have now written this bug twice. Also: **`run.sh`'s
output is block-buffered into a file**, so a log line can appear seconds after the process it came
from has exited - prefix with `stdbuf -o0 -e0` when a poller has to act on a line *while the
process is still alive*.

## My own time-wasting, written down so the next session skips it

None of these were Wine bugs. Every one cost a full test loop (8-25 minutes) or more, and all of
them are avoidable by checking one cheap thing first.

**I wrote the missing-latch bug twice in one session.** A run loop that polls
`ps ... | grep -q Microsoft.Media.Player.exe` and breaks when it does not match will break on the
*first* poll, because the app process does not exist yet during `run.sh`'s prefix setup. Fifteen
"runs" completed in 8 seconds each and reported "no fault"; I only noticed when I checked
`wc -l` on the logs. **Before believing a negative result from a loop, check how long each run
actually took.** The latch:

```
seen=0; gone=0
if alive; then seen=1; gone=0
elif [ $seen = 1 ]; then gone=$((gone+1)); [ $gone -ge 6 ] && break
fi
```

**I polled a log for a line and then acted on it after the process had exited.** `run.sh`'s output
is block-buffered into a file, so an `err:` line can surface seconds *after* the process that wrote
it is gone. My `/proc/<pid>/maps` capture came back empty twice for this reason, and I blamed the
`ps` pattern instead. Prefix with `stdbuf -o0 -e0` whenever a poller must act while the process is
still alive.

**I trusted `addr2mod.py` without checking `SizeOfImage`** and spent a round chasing
`mrt100_app.dll` and its PDB for a fault that was in `ucrtbase`. The offset was 4x the module size.
One `objdump -p | grep SizeOfImage` would have killed that immediately.

**I reused a module map across a rebuild** and concluded a correct fix had failed. Rebuilding
`ucrtbase` moved every base; the fault had simply moved to a new address in the same function.

**I killed my own shell with `pkill` for the fifth time** (exit 144). The `[s]oak2.sh` bracket
trick does not help when the plain string also appears elsewhere on the same line - in my case in
the `pkill -x soak2.sh` argument itself. See the rule above; build the pattern from pieces.

**I burned turns polling background tasks.** Re-reading a task's output file every few seconds
tells you nothing the completion notification will not, and the harness rejects the repeats
outright. Start the wait with `run_in_background`, then *stop* and do unrelated work - or nothing -
until the notification arrives.

**The Bash tool's shell is zsh, not bash.** `for p in $list` does not split words, `set -- $p` gets one
argument, and a stray `===` is a parse error. Anything that loops over pairs of coordinates belongs in a
`#!/bin/bash` script file. Also: the `/tmp/wheeltest.sh` count matches log lines by `+timestamp`, and
without that channel option it reports 0 whatever happened - I read one such 0 as a result.

**I asked `winmd.py --type` for `MediaBindingEventArgs`** and got silence, because `--type` matches
the fully-qualified name. `--grep` first, always.

## Environment facts worth knowing

- **`flatpak kill <app id>` also kills that app's build.** flatpak-builder's build sandbox runs under the manifest's
  app id (`io.github.designgears.Shapr3DLinux` in shapr3d-linux): on 10-11 a kill meant for an installed Flatpak's
  Shapr3D ended a `release.sh` run with `module installer: Child process exited with code 137`. Kill a stray app by pid.
- **A Flatpak instance cannot see another instance's processes** (each has its own pid namespace), so "is the app
  already running?" must not look in `/proc`. `wine-uwp run` holds a lock on `DATA/logs/.APP.running` and `activate`
  tests it; a `/proc` scan sent sign-in links to a second copy.

- **Every app blank with `ignoring FailFast 0x887a0004` (`DXGI_ERROR_UNSUPPORTED`) is the host's GPU stack, not us.**
  On 10-11 pacman had upgraded the kernel and NVIDIA without a reboot: `err:vulkan:init_physical_devices ... res -3`,
  `vulkaninfo --summary` finds no GPU (the other GPU is AMD, with no `vulkan-radeon` installed), and `/proc/driver/nvidia/version` (loaded module) differs from `pacman -Q nvidia-utils`. The desktop looks fine because it started before the upgrade. Check `vulkaninfo`
  first; until a reboot, `WINE_D3D_CONFIG=renderer=gl` (llvmpipe on Xvfb) still gives Calculator AE 0.

- **A bare `wine` is the *system* Wine (11.17 in `/usr/bin`), not ours - and running it on our
  prefix rewrites it.** `env.sh` exports `WINE_BUILD`/`WINE_BIN_DIR` but does **not** put the build
  on `PATH` (`run.sh` does that itself), and `WINE_BIN` is empty, so `${WINE_BIN}wine` outside
  `run.sh` is `/usr/bin/wine`. The system Wine sees a foreign `.update-timestamp`, runs a prefix
  update and copies *its* builtins into `system32` - a DPAPI probe then hit the old
  `NCryptCreateProtectionDescriptor` stub from `/usr/lib/wine` although the build tree had the
  implementation. Its symptoms: `version mismatch 964/961` against a running wineserver, and
  `setupapi:create_dest_file failed to create ... icu.dll` during a `wine reg query`. Always
  `export PATH="$WINE_BUILD:$PATH"` first, and check `which wine`. Our Wine redoes the update on its
  next start, which is what repairs it; the native byte-patched DLLs are not touched by either.

- `winedbg` crashes on startup in this build (but `probe-threads.exe` above gets any thread's context), `gdb` cannot attach (`/proc/sys/kernel/yama/ptrace_scope`
  is 1, and changing it is a system-wide setting - do not do it unasked), and `eu-stack` returns
  nothing useful for Wine threads. **Locate crashes by mapping the fault address from a `+loaddll`
  log with `addr2mod.py` and resolving it against the public PDB.**
- `.NET Native` apps (`ilc` in the PDB path, `SharedLibrary.dll` + `mrt100_app.dll` loaded) have no
  public PDBs, but `mrt100_app.pdb` does - and `RhpPInvokeExceptionGuard` in a stack means a native
  exception was taken under a P/Invoke, which is always a fail-fast. If you see that, look for a
  *native* fault, not a managed bug.
- A Wine DLL can have a **copy in the prefix** (`$WINEPREFIX/drive_c/windows/system32/`) shadowing
  the build tree. Restoring a backup there can silently reinstate a pre-fix build - check with
  `cmp` against `~/src/wine-build/dlls/<dll>/x86_64-windows/<dll>.dll` after any backup/restore
  dance.
- Leftover `explorer.exe /desktop` and `svchost.exe` from killed runs accumulate and make the
  activation race fail (run.sh says so in a comment). `wineserver -k` and *verify* they are gone
  before trusting a run.
- Wine's builtins win by module name. Pointing an `ActivatableClassId` at a native DLL is not enough;
  it also needs a `=n` override and the file in the prefix. The same holds for a Microsoft DLL that native code
  `LoadLibrary`s itself: `windows.ui.core.textinput.dll` loaded our builtin, and `GetProcAddress` for its ordinal 1500
  returned NULL without a word. Check `+loaddll` for `builtin` before assuming Microsoft's code runs.
- **A restarted test KWin (`kwin_wayland --virtual --xwayland`) may not take XTEST input**: on 09-27 `xdotool mousemove`
  left `getmouselocation` where it was and every click missed, although the same command had worked hours before.
  Scripted runs silently stay on Home (`shownframes.py` says `none`). Use Xvfb `:96` at 3440x1440 instead.
- **`xdotool` was removed once** (2026-09-25, a `pacman -Rns` clean-up) and the user reinstalled it; every
  `/tmp` test script drives the apps with it. If it is missing, find a window with
  `xprop -root _NET_CLIENT_LIST` plus `xprop -id <w> WM_NAME`, capture with `xwd -id`, and ask - it needs sudo.
  A `2>/dev/null` on an `xdotool search` hides the "command not found" and looks like "no such window".
- **Wine *sends* mouse-in-pointer `WM_POINTER*` from inside `peek_message`, while it processes the mouse message; Windows
  posts them as messages of their own.** So a CoreWindow's `WM_POINTER*` is never "retrieved": trace `ptrdisp` `sending`,
  not `got`/`peeked`. `GetPointerInfo` answers only for the current message's pointer (`current_pointer_id`), which
  that send sets. Anything new that keys off "the current message" must handle both routes.
- Wine's `GetObject( dib, sizeof(DIBSECTION) )` reports `dsBmih.biHeight` **positive** for a top-down DIB,
  so it cannot tell you the row order. Every DIB `dcomp/composition.c` creates is top-down.
- The unhandled-error text an app reports is often a *consequence*. `Delegate to an instance method
  cannot have null 'this'` was the second exception; the first, further up, was the real one.

## Current state (2026-09-29)

| App | State |
| --- | --- |
| Note (10-12) | `regress.sh sol` passes again (10-15: our `msvcp140_app` lacked `std::get_new_handler`). `regress.sh fh` still fails on the dev prefix (Feedback Hub blank, 16 fail-fasts): not yet looked at |
| Calculator | Fully working; the byte-identical regression baseline (249 colours, `magick compare -metric AE` = 0), and the keyboard baseline (`regress.sh kb`). Runs on unmodified CoreMessaging.dll, Windows.UI.dll and Windows.UI.Xaml.dll (no Windows DLL is patched any more) |
| Feedback Hub | Fully working and interactive (822 colours, 0 fail-fasts) |
| Media Player | **Plays audio from its own UI** - Open file(s) -> pick -> Play decodes WAV and MP3 through MF to `waveOut`. Needs `WINE_UWP_DRIVE_VIEW=run WINE_UWP_DRIVE_FIRST_VIEW=1 WINE_UWP_MANUAL_CONFIGURE=1`. Starts clean 15/15. Regression capture: 2250 or 2242 colours, both a pass - 2242 is XAML not showing the keyboard focus rectangle on Open file(s), usual since painting left the UI thread |
| Camera | Renders, but paints its own crash page: a `NotImplementedException` in a static constructor. Next target is `ITitleBarClientAdapter` `{ba5e7927-...}`, which our `wineuwp` navigation client refuses and `CoreApplicationViewTitleBar::_TryGetTitleBarClientAdapter` wants |
| Solitaire | **Playable on the unpatched `Solitaire.exe`** (`docs/screenshots/solitaire-unpatched-menu-game.png`): main menu -> Klondike -> game, and clicks move cards. No exe byte patch is needed any more: `wineuwp` delivers `CoreWindow.VisibilityChanged`. The Sign In Error popup shows on top of the menu; Cancel is at about (813,549). The blank-window start-up flake (about 1 in 6 through 09-24) is gone again, 0/20, after two fixes to combase's `CoMsgWaitInProcessEvents` (Findings-M4, 2026-09-24). Shapr3D's rarer UI stall is fixed too (Findings-M4: a dcomp object written into ntdll's futex bucket, and unserialised painting) |
| Windows Security | Blank; needs the Defender SecurityHealth service |
| Candy Crush Saga (Xbox app game) | **Reaches the title screen and a level board** (`docs/screenshots/candycrush-title-screen.png`, `candycrush-level-board.png`) on a private prefix, after two fixes (10-15): the StateRepository vtable was 64 slots for a 151-slot Package interface (`abort()` after the loading screen), and a `D3D11CreateDevice(NULL)` retry loop (fixed by the NULL driver using the hardware adapter). Both fixes are in the shared build now; the user plays levels from the Xbox app on `:0`. **In a landscape window the saga map gets a half-window side panel** (the nav bar hugely scaled, looks like a stuck texture); a portrait-shaped window is correct. Same on Vulkan and GL, not dcomp. Findings-M4 2026-10-15 (night, landscape panel) |
| Xbox app (GamingApp) | Staged as `xbox`. On the **builtin** `windows.web` (no native Windows.Web.dll) it reaches its own home screen after **Browse app** on the sign-in gate (`docs/screenshots/xbox-home-builtin-windows-web.png`). Gaming Services and Windows.System.User are stand-ins. Still open: the nav icons for My Library, Cloud Gaming and Store, the faded banners, sign-in, and the Store hero video. Findings-M4 2026-10-14 (night). **Sign in** (10-15): `dlls/winexboxauth` (C++ `shims/xboxauth` until 10-21; traces `WINEDEBUG=+xboxauth`) stands in for `XboxAuthManager` with a Microsoft account device-code sign-in in the user's browser (`microsoft.com/link?otc=<code>`) and real Xbox Live user/XSTS tokens; verified up to the device code and the browser launch, the account step needs a human. Refresh token in `%LOCALAPPDATA%\wineuwp\xboxauth.bin`. Findings-M4 2026-10-15. **Requests are signed** (10-15, ported from xal-rs): a per-prefix P-256 proof key, a real device token, SISU authorize, device/title-bound XSTS and a `Signature` on every token request with a policy; the signing is proven against `device.auth.xboxlive.com` (signed 200, unsigned 400, one bit flipped 403). SISU with a real account works (10-15, Android device token). **Store account and Gaming Services** (10-15): `dlls/winexboxauth` also serves `WebAuthenticationCoreManager` and `StoreConfiguration` (the Store account is the same Microsoft account; its WebAccount.Id is the PUID), so Settings > Account has no mismatch; `dlls/winegameplatform` (C++ `shims/gameplatform` until 10-21; traces with `WINEDEBUG=+gameplatform`) covers every Gaming Services client class (traced, table from `gen-gameplatform.py`) with install locations, update settings, `Package.Create`, `Store.QueryProductDetails` and content access, so Install Options loads and game pages show Get/Buy. Findings-M4 2026-10-15 (Store account). **Friends list and bright game pages** (10-15, dcomp facade expressions, dwrite). **Installs and plays a game** (10-15 night): Get opens the real Store purchase dialog through a WebView2 runtime from the ISO (`dlls/webview2host`, C++ `shims/webview2` until 10-21, hosts it windowed in a popup; traces `WINEDEBUG=+webview2`; shown on screen and closable on 10-21, `docs/screenshots/xbox-purchase-dialog-on-screen.png` - an earlier "black on screen" was `xwd -root` misreading the popup, see the capture trap below), Install downloads through FE3 and `install-app.py` into `uwp-bin/xbox-games/stage`, the page shows Downloading then Play, and Play starts Microsoft Solitaire Collection 4.27 (`docs/screenshots/xbox-install-downloading.png`, `xbox-install-ready-to-play.png`, `xbox-play-launches-solitaire.png`). Run the Xbox app with plain `err+all` when testing Play: the game inherits `WINEDEBUG`, and `trace+tracelogging` makes Solitaire fault. Open: library box art. Pause/Resume retains the queue row, displays Download paused and offers Resume; three virtual-desktop UI cycles preserve offsets with stable installation/package identities. Queued Package.QueryDetails must return unregistered, not fully streamed metadata: GP_E_NO_SERVICE there aborts the native paused-state update (Findings-M4 2026-10-16, paused-row investigation). `install-app.py` now locks staging per package and cleans only its own scratch entry, preventing failed installs from deleting another install's work. A synthetic stale-file repro and virtual Halo install passed the former failure point; the queue reached Download paused and Resume worked at 31% (`docs/screenshots/xbox-install-staging-paused.png`; Findings-M4 2026-10-16, install error). The desktop app was relaunched with the rebuilt shim on 10-16. Premium, Game Pass and direct Win2D geometry marks now share bounded supersampling; scaled composition surfaces use source-footprint filtering. Verified on virtual desktops with fill/stroke, clip/transform/DPI probes; the tested D2D/DComp DLLs are in the standard Wine build. Desktop Home was visually confirmed after relaunch with these DLLs (Findings-M4 2026-10-16, Game Pass). **Manage > Add-ons Apply changes** (10-16): fixed the installed-drive lookup. Package.QueryInstallPath returns Z:, but storage enumeration previously exposed only C:, so the React Native Apply callback returned at its installed-drive Content guard. Storage now exposes the actual host data drive and filters content by device. Two virtual UI runs select Campaign, close Manage on Apply and queue PackageRequest.ConfigureFeatures(CampaignS1); this uses StartItemInstall, not PackageUpdate.StartUpdateAsync. Tested at 0 B, paused and canceled rather than downloading the whole payload (`docs/screenshots/xbox-campaign-apply-queued-paused.png`). General's update-priority control also loads and persists through Customization.QueryGameProps/SetGameProps (`docs/screenshots/xbox-game-priority-persisted.png`). **Cancellation** (10-16): cancel now signals the real host Python PID and waits for the shell EXIT record; terminating only the Wine launcher had left a paused Python child alive. The virtual UI cancel test emptied the queue, logged EXIT 143 and confirmed that exact Python PID was gone (`docs/screenshots/xbox-campaign-change-canceled.png`). The host installer reports its PID only for installs; --info remains JSON. Findings-M4 2026-10-16 (Apply/host cancellation). **MSIXVC (GDK) packages**: the package list, CDN download, content keys (our own Windows device licence, `dlls/winexboxauth/windevice.c`) and our GDK runtime all work: see the Bad Magpie and Halo rows. Xodus was only a protocol reference and is no longer kept here; the Xodus Nexus launcher was tried on 10-19 and dropped (it plays only from its own streaming container). Findings-M4 2026-10-15 (night) |
| Halo Infinite (GDK, Xbox app install) | **Campaign plays without HD1** (10-18, `docs/screenshots/halo-campaign-in-game.png`): launch with `UWP_STAGE=254428597cfe2 WINE_BUILD=uwp-bin/wine-build-wow64 WINE_HACK_NATIVE_SYSCALLS=1` on `uwp-bin/prefix-pause-ui30`, title -> Campaign -> New Game -> map. **Multiplayer matchmakes and plays online** (`docs/screenshots/halo-multiplayer-in-match.png`). Sign-in uses the Store account; the Store-user mismatch notice at start-up is still open. Findings-M4 2026-10-18. **From a fresh prefix** (10-19, `docs/screenshots/halo-fresh-prefix-main-menu.png`): `new-prefix.sh`, Xbox app install, `install-d3d-runtime.sh`, then Play (`wineuwp-launch.exe`) reaches the online main menu with no hand-made files |\n| Bad Magpie Demo (GDK, Game Pass) | **Installs from the Xbox app and runs, in game** (10-15, `docs/screenshots/badmagpie-gdk-running.png`): the first GDK title on Linux here. The `.msixvc` comes from `packagespc.xboxlive.com` (signed XBL request), its content key from our own Windows device licence and `licensing.mp.microsoft.com` (`dlls/winexboxauth/windevice.c`), and the installer (`dlls/appxinstall`, called by `dlls/winegameplatform`; formerly `install-app.py`) decrypts the XVD (XTS per 4096-byte page, data unit from the L0 hash entry) and lays the files out from `SegmentMetadata.bin`. Reinstalled through the C installer on 10-19 (`docs/screenshots/badmagpie-c-installer-running.png`). EAppx (Resident Evil 7) uses the same licence path and the EXBH/EXPH reader; its install is not yet run end to end. **The device registration takes a slot in the account's device group (it was full once); one per prefix, kept in `xboxauth.bin`** |
| Packaging | **One AppImage**, in the separate `../shapr3d-linux` repo since 10-11 (github.com/designgears/shapr3d-linux; its README; the runtime it carries is `packaging/` here). Bottles was dropped on 10-11. One freedesktop SDK build (flatpak-builder) makes both the AppImage and a Flatpak bundle (`io.github.designgears.Shapr3DLinux`), both attached to each release; glibc 2.38 floor. The installer is Rust + Slint, styled after Shapr3D, with no secondary text (`../shapr3d-linux/docs/screenshots/installer/`); the AppImage was re-run on 10-11 from an empty data directory with the real ISO download and the real Shapr3D bundle, to the sign-in screen on `:0`. `appimage/smoke.sh` sets an AppImage up from an empty data directory on its own Xvfb and requires Shapr3D's first screen (passed on 10-11 in 27 s). Opening the AppImage (or `wine-uwp` with no command) shows the installer, which once set up is the launcher: version, Launch and Update... (pick a newer package; `wine-uwp setup PACKAGE` on an installed setup goes straight there). The package may be the vendor's `.appinstaller` (Shapr3D's download page hands one out): only its main bundle is used, read from the server with range requests (1.8 GB streamed in ~50 s, no temporary file), never its `<Dependencies>`; frameworks always come from the ISO, falling back to an older version than the package asks for. Its URI is kept in `stage/APP/.wine-uwp-appinstaller`, and the launcher page offers `Update to X` when it serves a newer version. **Shapr3D 26.180 needs VCLibs 14.0.33728 (STL 14.40, constexpr `std::mutex`); the ISO's 14.0.30704 `msvcp140_app` NULL-derefs in `_Mtx_lock`.** Our Wine builds `msvcp140_app` from msvcp140's sources and `launch-defaults.env` sets `msvcp140_app=b` (Calculator AE 0 and `kb` identical on it). An update unpacks to `stage/APP.new` and swaps it in at the end, refuses while the app's run lock is held, and keeps sign-in and projects (they live in the prefix under the package family name). The menu entry (`run APP`) is written on install and refreshed each time the installer opens. A plain `wine wineuwp-launch.exe` starts an installed app with no `run.sh`. Released through `release.sh` up to 0.0.3-beta (10-12), which the user checked on `:0`: launch, browser sign-in, update. **An AppImage started from inside another (the launcher's Launch is `$APPIMAGE run`) must not re-save the host environment**: in 0.0.2 it saved the launcher's bundled paths, xdg-open then got a dead mount's libraries, and the app ran on the system Wine and outlived its wineserver as a windowless orphan holding the run lock. `AppRun` keeps the first `WINE_UWP_HOST_*`. `wine-uwp stop` closes a running app through `logs/.APP.stop` |
| Microsoft Store | Staged as `store` (prefix `uwp-bin/prefix-store`, build `uwp-bin/wine-build-wow64`, the Media Player switches). **Updates itself in-app**: Library > Get updates goes through `shims/appinstallmanager` (catalogue + FE3 + `appxinstall.dll` into its own stage; 22401 -> 22609 again on 10-20, `docs/screenshots/msstore-c-installer-updated.png`) and installed 22608.1401.7.0 over the ISO's 22401 (old stage kept as `stage/store.pre-update-22401`). 22608's Home page paints real content (`docs/screenshots/msstore-22608-home.png`); start-up takes ~2-3 minutes; Apps was still on skeletons 45 s after a click. Findings-M4 2026-10-19 |
| Shapr3D | **Models, syncs and exports** (`docs/screenshots/shapr3d-extruded-body.png`): sketch -> extrude -> Visualization, toolbar and menus, cloud upload of revisions and thumbnails, **File > Export to > STL** and **Drawings -> Create Drawing -> Export drawing (PDF)** both write correct files (`docs/screenshots/shapr3d-drawing-pdf.png`). The file pickers (`dlls/windows.storage.pickers`) show Wine's common Open/Save/folder dialogs (starting in Documents, or the app's SuggestedStartLocation); `probe-pickers.exe` checks them without an app. For scripted runs, `WINE_UWP_PICKER_FILE` / `WINE_UWP_PICKER_SAVE_FILE` still bypass them, and `WINE_UWP_PICKER_NODIALOG=1` makes an unstaged pick report a cancel. On `:98` the dialog is its own top-level window ("Open" / "Save As"), captured with `xwd -id`. Type into it with plain `xdotool type` after a click, not `--window`. The colon needs `xdotool key colon`, and Ctrl+A does not select the combo box text. Signs in once (Log In -> `./browser-callback.sh shapr3d`, a human solves the reCAPTCHA, run with `WINE_UWP_LAUNCHER=log`) and stays signed in. Project thumbnails and material swatches show (`docs/screenshots/shapr3d-project-thumbnails.png`, `shapr3d-used-material-swatches.png`). The save dialog offers the app's `FileTypeChoices` (Screenshot -> Capture: JPEG, PNG; `docs/screenshots/shapr3d-capture-output.png`). **The prefix's `Documents`, `Desktop` and `Pictures` are symlinks to the user's real home folders**, which is where the dialogs start - save test output to `C:\users\Public\` instead. Share -> copy link puts the project URL on the clipboard (`docs/screenshots/shapr3d-share-copy-link.png`); the share-sheet button does nothing (there is no share host). Typed dimensions work while sketching (`docs/screenshots/shapr3d-dimension-entry.png`); drive them with `xdotool type --delay 250`, because at the default 12 ms the key after the one that opens the field is lost. Start -> Visualization -> import Drone passes **30/30** (`docs/screenshots/shapr3d-import-soak-30of30.png`); it was about 3 in 4 before 09-24. Leaving the editor no longer kills it: 30 leaves in 10 runs, against 6 deaths in 10 runs before (`docs/screenshots/shapr3d-home-after-leaving.png`). Extrude/offset drags on the drone at the user's 3440x1366 no longer starve the UI thread (it used 1.95 s of CPU in a 2 s drag, now ~0.3 s; the result lands ~0.3-0.8 s after release instead of 1-2 s); with continuous input the drone's preview refreshes every ~78 ms, bound by Parasolid (Wine ~11% of it), see Findings-M4 2026-09-25. Menus open with Windows' own animation now (key frames run live, with their easing, and the reveal clip slides with them): the first frame ~45 ms after the press, mostly out by ~60-90 ms, settled ~300 ms (`docs/screenshots/shapr3d-file-menu-reveal.png`). History chevrons turn > to v and back (`docs/screenshots/shapr3d-chevron-turn.png`). Wheel over a panel no longer reaches the viewport (it did ~1 in 15 at 175 Hz before 09-25's hit-test fix; 0 in 600 since). History moving after Edit... -> Cancel is the app re-creating its uncached page and scrolling the selected row into view (Findings-M4, 2026-09-26). The view cube popup lays its page out below its tabs from the first frame (`docs/screenshots/shapr3d-viewcube-open.png`). The viewport keeps moving while the editor is built after a project opens (worst gap ~60 ms, was 250-350) and runs at 166-176 frames a second on the user's own 175 Hz display, measured there (`docs/screenshots/viewport-after-open-round2.png`; the X server keeps up, `trace+x11perf`). Cloud sync no longer kills it: `CoWaitForMultipleObjects`, called by Windows.Web.Http, is implemented. Materials drag and drop onto the model with the dragged row following the pointer (`docs/screenshots/shapr3d-drag-visual.png`; `dlls/dataexchange/dragoperation.c`, an in-process drag loop), XAML keeps repainting afterwards, and the swatches no longer flicker grey. Flyouts, menus and dropdowns are windows of their own (painted by dcomp's `hwnd_target`s), by default since 09-28; `WINE_UWP_WINDOWED_POPUPS=0` turns that off. They used to cause a blank start-up about 1 time in 8, which was the missing CoreMessaging wake. The Share button keeps a 10 px margin from the window edge, lined up with the panels (`wineuwp` pushes the title bar insets; `docs/screenshots/shapr3d-share-inset.png`). Selecting a folder no longer draws the rebuilt toolbar at the window origin, and Settings sections cross-fade instead of overlapping (`docs/screenshots/shapr3d-settings-crossfade.png`). |

The three Media Player switches are opt-in only because the other apps have not been tested against
them. **Testing that, and making them the default if it holds, is a standing job** - they change the
view/activation ordering for everyone, which is exactly the sort of thing Shapr3D will also depend
on. `WINE_UWP_MANUAL_CONFIGURE` is **load-bearing for Media Player** and cannot simply be dropped -
without it the app never paints (8 runs, 8 no-paint), because `broadcast_configure_core_window`
returns before both the direct call and the APC broadcast. They are **irrelevant to Solitaire**:
all three unset behaves identically. Note the partial combinations (`MANUAL_CONFIGURE` alone,
`DRIVE_VIEW=run` alone) fail to bootstrap Solitaire at all - the switches are not independent.

The `ConfigureCoreWindow` hang that used to lose one run in three is **fixed** - it was our own
APC landing on Wine's RPC server thread, not anything in XAML. See Findings-M4.md.

### Solitaire: the 1x1 render surface is real, and is not the reason nothing draws

`Setting Render Window - Surface is 1 x 1, DpiScaleFactor is 1` is logged **exactly once** per run
and never updated. The chain is fully mapped: `Dx11RenderWindowXaml::Initialize` (`0x851134`) reads
the SwapChainPanel's `get_ActualWidth`/`get_ActualHeight` (`IFrameworkElement` slots 13 and 14, the
calls at `0x8511c9` and `0x85122e`); `XamlBootStrap::SetSwapChainPanel` runs before XAML's first
layout, so both are 0; and the one path that would fix it later - the `CoreDispatcher`
`DispatchedHandler` posted by `XamlBootStrap::OnInitRender` (`0x7de1a0`) - early-returns because
`SetSwapChainPanel` already set `[this+0x48]` and `[this+0x50]`.

**Forcing the two reads to 1280.0/800.0 gives a correct 1280x800 surface and changes nothing on
screen.** Keep the finding, drop the conclusion. The real fix is still to deliver the CoreWindow
visibility/activation event, which would also set `+0x20a` and retire the `0x7dee64` patch.

**The alternating screenshots are our own byte patch.** Six captures two seconds apart gave
151742, 151118, 151852, 380, 151713, 151118 colours - the splash artwork and the popup's bare
gradient in alternation. That is the forced frame loop presenting frames the app never drew into,
not evidence about the app.

### Solitaire: the async-callback and socket-locking rules

Two rules that came out of implementing `StreamSocket`, both of which produce a *working-looking*
run that still fails, and both of which apply to any new WinRT async in Wine:

- **`async_operation_callback` runs inline on the caller's thread unless it returns
  `STATUS_PENDING`.** `async_info_complete( impl, FALSE )` calls it directly and only submits the
  threadpool work item if it gets `STATUS_PENDING` back. Any callback that blocks needs
  `if (!called_async) return STATUS_PENDING;` as its first statement, or the "async" operation
  completes before the caller has done the next thing it meant to do.
- **Never hold one lock across both the receive and send paths of a socket.** An HTTP client posts
  its read before sending the request; with a single lock the read blocks in `recv()` and the
  request is never sent. `Close()` and reconnect need the receive lock a blocked `recv()` holds,
  so they `shutdown()` the socket first, unlocked, to wake it.

### Solitaire: how it was made to render, and what the dead ends were

**The 1x1 swap chain was never the bug**, and it is still logged as 1x1 in runs that render
perfectly - `Dx11RenderWindow::UpdateWindowRect` resizes it silently, logging only on failure. Two
sessions were spent on that line. What actually stopped it was a chain of eight blockers, each
found by probe and each moving the app one step further:

1. `install-overrides.sh` racing itself, so the shims below were registered and never loaded.
2. `DeviceInformation::CreateWatcher(DeviceClass)` -> `E_NOTIMPL`, and then the watcher's own
   `add_Removed` -> `E_NOTIMPL`. C++/CX turns both into throws.
3. `TaskbarManager` -> ImmersiveShell: an `IAsyncOperation` never completed.
4. The advertising-id apiset: an unresolved delay import raising `EXCEPTION_WINE_STUB`.
5. `StoreContext` -> ContainerHostActivation: same never-completing async. Only `GetDefault` is
   ever called.
6. `GetPackageFamilyNameFromToken`, repaired by patching the delay-import thunk from `wineuwp`.
7. `WebAuthenticationCoreManager` -> the token broker: same shape again.
8. `SpatialAudioDeviceConfiguration` and `SpatialAudioFormatSubtype` answered `E_NOINTERFACE` by
   Wine's `windows.media.devices` factory. Wwise asks with `.as<>()`, which throws, on its own
   audio thread - **this was the one that killed it after the splash screen appeared.**

Ruled out by probe, so do not re-walk them: XAML **does** raise `SizeChanged` on the SwapChainPanel
and the dispatched handler **does** run; `CoreDispatcher.RunAsync` works;
`XamlBootStrap::OnInitRender`'s early return is benign; the three Media Player switches are
irrelevant (all three unset is identical). Two wrong turns worth remembering: I nearly reported
"cross-thread `RunAsync` is dropped in combase" (a probe disproved it before I touched another
session's file), and an `xdotool windowsize` test gave a clean negative that meant nothing because
the resize never reached XAML at all.

Next: the app holds on the splash page. The CDN explanation first written here was **wrong** - the
app had no working HTTP at all (msxml6's XMLHTTP classes were never registered, so libHttpClient's
IXMLHTTPRequest2 backend could not create its transport), and with that fixed it logs
`PlayFab login successful`. It still stops, but **not in the bootflow** - that was the wrong
layer twice over. `BootflowItemFont` was named here and then disproven (forcing its gate open
changes nothing, and its font future *does* succeed). The reason items 3, 5 and 7 "provably never
begin" is that **nothing was ever ticked**: `+0x20a` was unset, so exactly one frame ran. With the
frame loop running the bootflow completes in full. The rendering path itself is proven end to end,
which is the part Shapr3D needs.

**Beware ~1-in-3 run variance**: some Solitaire runs stall before `App -- AppState Transition ...
'Sgi::BootflowState'` with no probe applied at all. A probe that "did not fire" in such a run means
nothing - check the run reached the bootflow first.

**Xbox install-queue artwork is a separate unresolved binding.** On isolated Xvfb `:97`, Halo's
manifest logo is present in the active `.new` directory and is a valid 44x44 PNG. The small
Downloading tile at bottom left renders it, but the 100x100 image in the Install queue row stays
blank. The app calls `Package.QueryDetails -> pending`; neither `Square44x44LogoPath`,
`Square150x150LogoPath`, nor `IsUwp` is queried in the same trace. Thus the missing large queue
image is not an absent/bad staged logo or these PackageDetails getters. Keep the test install paused
at low progress while tracing the queue row's actual image source; do not download the full game.
Screenshot `/tmp/xbox-icons-current.png`, log `/tmp/xbox-icons-isuwp-test.log`.
A `WINEDEBUG=+file` follow-up confirmed that Xbox does not open the staged Halo icon path during
app startup. It also showed a restart limitation: the UI queue is empty after relaunch even while
the installer process remains paused. Reproduce the active row before drawing conclusions from
file-open tracing. Two WIC scaler warnings fall back to nearest-neighbour on unsupported source
pixel formats: relevant to blocky scaling, not evidence for the blank queue tile.

The user's `:98` run preserves the active-row reproduction in `docs/screenshots/xbox-install-queue-blank-art.png`
at 843.9 MB paused. Its Gaming Services log again has `Package.QueryDetails -> pending` and no logo
property calls. The test item is GDK/XVC (`not UWP`); trace which Xbox image service feeds that row.
The product-detail page paints store artwork, so general network or image rendering failure is not
the cause. A fresh paused-queue reproduction shows React Native dispatching `ImageCache.prefetchItems`,
but the image-cache DB stays empty; `PackageDetails` logo getters and `FileImageElement` load errors
remain absent. Cached Halo product records have multiple artwork URIs, and a dispatch follows install
start for the same product ID. Do not use the earlier breakpoint at `XboxPcApp.exe+0x6f9ea0` as proof
of that dispatch: code inspection places its caller in a generated `StorageItemThumbnail` async
operation, so its prior `prefetchItems` label was unsupported. A fresh My Games capture has 602
entries with placeholders; WinHTTP receives a 424px PNG with HTTP 200 and WIC independently decodes
it. A later file trace sees ImageCache temp files written and renamed to final names, while the DB
still has zero rows; it shows no read of those final image files before the fallback. The app logs
`FileImageElement::LoadImageSource` / `BitmapImage.SetSourceAsync` failing with `E_FAIL`. The focused
probe confirms `StorageFile.GetFileFromPathAsync`, `OpenAsync`, and a 134,781-byte random-access stream
work, but a standalone process cannot activate `BitmapImage` (`RPC_E_WRONG_THREAD`), so it does not
validate `SetSourceAsync`. Next trace the stream inside Xbox's actual UI thread and connect the
cache finalization to its lookup. Evidence is in Findings-M4, “Halo install-queue artwork”.

**Xbox library artwork still shows placeholders.** My Games reports 602 titles and the image
endpoint returns PNG data, but the visible tiles use fallback art. App logs report
`GameDetailsImageLoadError/50079` (`E_FAIL`) in `FileImageElement::LoadImageSource`. Several
XAML stream and decode samples returned success, but they were not evidence about the original
artwork. Pointer correlation shows the sampled `SetSourceAsync` actions and matching
`GetResults` calls occur after those errors and complete with `S_OK`; they are the app's bundled
fallback images. The original-art failure is earlier in the load path and remains unlocated. Earlier
file traces saw cache temp files renamed to final names, but no reads of those final paths before the
fallback; `ImageCache.db` had no rows. Do not infer a WIC failure or successful original-art decode
from the fallback actions. See Findings-M4, “Halo install-queue artwork” and “Async BitmapImage
callsite correlation” (10-16).

**EAC runtime installation is separate from EAC compatibility.**
`WINEPREFIX=... packaging/install-eac-runtime.sh --download` installs/links the Proton runtime without
restarting Wine. env.sh supplies both PROTON_EAC_RUNTIME and WINEDLLPATH: our Wine does not interpret
the former itself. Keep its PE DLLs with their Unix .so partners. The original shared build is x64-only;
`uwp-bin/wine-build-wow64` now builds both architectures from our source (use --enable-archs=i386,x86_64
--enable-wineuwp=x86_64; the UWP host itself uses x64 diagnostics). The desktop prefix-test was updated
with that build's wineboot -u. Halo's bundled 32-bit EOS installer then exited 0 and registered
EasyAntiCheat_EOS with its executable under Program Files (x86). Protected-game compatibility is still
not established. The later Halo launch investigation fixed EAC module mapping (below). Never mix
another Wine version's 32-bit builtins into this prefix. See Findings-M4,
“Proton EasyAntiCheat runtime installation” (10-16) and “Building WoW64 for the Xbox prefix” (10-17).

**Halo's EOS bootstrapper needs native Linux syscall interop.** The 206 mapping failure was
`memfd_create` from its 0x700100000000 Linux helper being interpreted as a Windows syscall.
Our ntdll now supports Proton's WINE_HACK_NATIVE_SYSCALLS=1 opt-in for that native address range;
run.sh and the installed game's launch.env enable it for EOS launchers. Crucially, read this flag
from the child's Windows environment at signal_init_process too: SetEnvironmentVariable in
wineuwp-launch does not update the host environ inherited by posix_spawn. The direct and actual
wineuwp-launch paths now reach EAC status 301 (successfully loaded in-game), including on :0.
Halo then reports missing Hardware Tiled Resources Tier 2 graphics support. Online play is untested.
See Findings-M4, “Halo EAC 206 mapping failure, native syscalls” (10-17).

**Updating a finished Unix module while apps remain open must preserve their mapped inode.**
For the tested ntdll fix, compiled only its Unix objects, linked to a separate temporary .so and
used an atomic rename into the build. Do not truncate/relink a mapped .so in place. Experiments
still belong in a separate build; atomic installation is for an already-tested change.

**Do not rewrite an active launch script in place.** Bash can resume reading run.sh after its
long-running Wine command exits. Inserting lines earlier in that same file while it waits can
leave its read offset in the middle of a comment, producing a bogus unmatched-backtick/EOF error
when the old app is stopped. The current script may pass bash -n throughout. Apply script edits
by atomic replacement (new inode), or after its active invocations finish.
