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
| `run.sh`, `install-overrides.sh`, `stage-app.sh` | launching and prefix setup |
| `shims/`, `probes/`, `CMakeLists.txt` | the WinRT stand-ins and the diagnostic probes |
| `*.py` | the investigation toolkit (`xref.py`, `winmd.py`, `piid.py`, `fasthive.py`, ...) |
| `wine/` | **our Wine source**, vendored in full by `sync-wine.sh` - 196 files, +49k lines vs upstream |
| `docs/` | Plan, Findings, screenshots |
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
- **Run on Xvfb `:98`, never the user's `:0`.** `DISPLAY=:98 ./run.sh`.
- **The Wine tree (`~/src/wine`) is ours, all of it.** It is simply a checkout that lives outside
  this repo. Edit anything in it, then `./sync-wine.sh --from-tree` and commit the result here.
  Earlier revisions of this file claimed the tree was "shared with another session" and listed
  `dlls/kernelbase`, `dlls/ntdll`, `dlls/win32u`, `dlls/user32`, `dlls/combase`, `dlls/d3d11`,
  `dlls/windows.graphics`, `dlls/wintypes`, `server/` and `include/` as off-limits. **That was
  false** - every commit in the tree is ours - and it cost real work; see the mistakes section.
- **Never push.** Commit freely; pushing is the user's call.
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
  `wine/FILES` is an **explicit list, never a glob**, so this cannot start sweeping the other
  session's work into our history.
- **This used to be a patch export, and it silently lost ~95% of the work.** Two compounding
  causes: `git diff` sees only the *uncommitted* delta, while most of the work was already
  committed in that tree; and the exporter's file list came from the bogus "not yours" claim
  below, dropping another ~5,000 lines. It captured 2,700 lines and looked complete. If you ever
  find yourself exporting diffs of our own code again, stop.
- **Byte patches go in `$UWP_BIN/win11/...`**; `run.sh` re-copies into the prefix. Always keep a
  backup and verify the restore with `cmp` before moving on. Leaving one behind silently poisons
  every later run - it has happened.
- **Never patch the last pristine copy of a Windows DLL.** Three DLLs are patched today
  (`CoreMessaging.dll`, `Windows.UI.dll`, `Windows.UI.Xaml.dll`, all 26100.8972); their originals are
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
  `wineuwp.reg` now carries an `@UWP_BIN_WINPATH@` placeholder that `install-overrides.sh`
  substitutes at import time.
- **`build.sh` fails with a stale CMake cache after a move.** `uwp-bin/build-probes/CMakeCache.txt`
  records the *old* source and binary directories (`/home/bobby/Projects/calculator/tools/wine-uwp`,
  `$HOME/uwp-bin/build-probes`) and CMake refuses to reconfigure. `rm -f
  uwp-bin/build-probes/CMakeCache.txt && rm -rf uwp-bin/build-probes/CMakeFiles` then `./build.sh`.
  Back up `uwp-bin/build-probes/*.exe` first if you are mid-investigation - they are the only copies
  until the rebuild finishes.
- **`identity.env` in a stage directory must not contain `WINE_UWP_PACKAGE_PATH`.** `run.sh` sources
  that file, so a stale value there wins over the computed one. `run.sh` now always computes it from
  the stage directory and `stage-app.sh` no longer writes it.

**`info[0]=0x8007007B` in an exception record is `ERROR_INVALID_NAME`** - "the filename, directory
name, or volume label syntax is incorrect". It means a path is *malformed*, not missing, which is a
much narrower thing to look for: an unexpanded glob, a doubled separator, an empty segment. Trace
with `WINEDEBUG=+file` and read the last named `CreateFileW`. That one number turned a day-shaped
hunt into a ten-minute one, after I had already wasted four runs checking that paths *existed* -
they all did.

## The regression protocol - do not skip it

Every change to Wine or the host gets checked against apps that already work, *before* you believe
a result:

```
# Calculator must stay byte-identical to the saved baseline
DISPLAY=:98 UWP_TIMEOUT=80 UWP_LOG=/tmp/c.log ./run.sh &  sleep 48
DISPLAY=:98 import -window root /tmp/c.png; cmp /tmp/c.png /tmp/calc-regress.png

# Feedback Hub: no fail-fasts, no app error, ~822 unique colours
DISPLAY=:98 UWP_STAGE=windowsfeedbackhub ... ; identify -format '%k' /tmp/fh.png
```

`identify -format %k` (unique colours) is the cheap "is it painting" metric: 1 = blank, hundreds =
real UI.

**Run anything flaky twice.** A change that produced 66 fail-fasts in one run and 6 XAML
activations in the next looked fine in its first run. Single runs are not evidence.

## Investigation toolkit

These all exist and work. Prefer them over reading code and guessing.

**Run the Python tools with `$UWP_BIN/venv/bin/python`, not `python3`.** Six of them import `pefile`
or `regipy`, which are installed only in that venv: `disas.py`, `import-audit.py`,
`ordinal-audit.py`, `pdbsyms.py`, `xrefs.py`, `gen-classes-reg.py`. Under a bare `python3` they
die with `ModuleNotFoundError` and look broken. This file previously asserted that `pefile` was
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
| What is the visual tree doing? | `WINEDEBUG=+dcomp`; `present_desktop_hwnd frame: N ops` - 0 ops means nothing to draw |
| Why is an app stuck, when the stack dumper cannot say? | `WINE_UWP_PROFILE=<prefix> WINE_UWP_PROFILE_MS=10` - a sampling profiler already in `wineuwp`. Writes `<prefix>.<pid>` every 2 s: an `M` line per module then `S <tid> <rip> <count> <4 callers>`. It **found the waiting thread's loop when `WINE_UWP_DUMP_STACKS` returned nothing useful**, and it distinguishes *blocked* from *spinning*, which no single stack snapshot can |
| What is every thread actually doing? | `WINE_UWP_DUMP_STACKS=<seconds>` - suspends and unwinds every thread, prints `STACK tid N frame M rip=`. Works on an *idle* app, not just a hung one. Resolve against `/proc/<pid>/maps` from the same run |
| Method count of a private interface | parse the MIDL proxy/stub DLL, below |
| Where is the code behind one of the app's own log strings? | `./xref.py <exe> --string '<log string>'` - finds rip-relative LEAs to it and resolves each to its **enclosing function** via `.pdata`, which is where a `ud2` has to go. `--callers <rva>` finds direct call sites |
| Does a size change reach the app? | `$UWP_BIN/build-probes/probe-resize.exe <w> <h> [title]` - `SetWindowPos` through Wine. **`xdotool windowsize` does not reach Wine at all** |
| Is this Windows DLL one we have byte-patched? | `./pecheck.py <file>...`, or `./pecheck.py --sweep uwp-bin/win11` to list every modified file. Authenticode + PE checksum; needs no reference copy |
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

**Patch and restore *every* copy.** `run.sh` copies `$UWP_BIN/win11/...` into the prefix, so a
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
3. Best of all: find the **shipping implementation's vtable** and let its PDB name every slot. That
   is how `{9d47a108}`'s 13 methods were recovered - the 19-entry `MediaPlayerImpl` vtable in
   `Windows.Media.Playback.MediaPlayer.dll`. Do this before writing a single stub.
4. Failing all that, implement the interface with one traced thunk per slot and let the app tell you
   which methods it calls.

## Mistakes already made - do not repeat them

**Byte-patching a Windows DLL is fine; patching our own code is not.** We have no source for
those binaries, so a byte patch is the only tool there is - `CoreMessaging.dll` and
`Windows.UI.dll` carry load-bearing ones today and that is a legitimate, permanent state. Our own
Wine source is the opposite case: it is vendored in full under `wine/` and there is never a reason
to represent it as a diff.

What *is* true of a DLL patch is that it **cannot be shipped or re-applied as a diff** - that needs
the exact base binary, and Microsoft serves only current revisions. So the thing that matters is
keeping the pristine original (see the byte-patch ground rule) and recording what each patch does
in Findings, not converting it into something else. Re-home one into Wine when the Wine-side fix is
worth having on its own merits - most of the CoreMessaging patches are IOCP/message-pump behaviour
that Wine arguably should get right anyway - but that is an improvement, not a debt. See
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
`install-overrides.sh` now refreshes it and says `refreshed: stale wineuwp.dll in the prefix`.

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

**I asked `winmd.py --type` for `MediaBindingEventArgs`** and got silence, because `--type` matches
the fully-qualified name. `--grep` first, always.

## Environment facts worth knowing

- `winedbg` crashes on startup in this build, `gdb` cannot attach (`/proc/sys/kernel/yama/ptrace_scope`
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
  it also needs a `=n` override and the file in the prefix.
- The unhandled-error text an app reports is often a *consequence*. `Delegate to an instance method
  cannot have null 'this'` was the second exception; the first, further up, was the real one.

## Current state (2026-09-21)

| App | State |
| --- | --- |
| Calculator | Fully working; the byte-identical regression baseline (249 colours, `magick compare -metric AE` = 0). **Depends on hand-patched `CoreMessaging.dll` and `Windows.UI.dll`** - see the byte-patch rule above |
| Feedback Hub | Fully working and interactive (822 colours, 0 fail-fasts) |
| Media Player | **Plays audio from its own UI** - Open file(s) -> pick -> Play decodes WAV and MP3 through MF to `waveOut`. Needs `WINE_UWP_DRIVE_VIEW=run WINE_UWP_DRIVE_FIRST_VIEW=1 WINE_UWP_MANUAL_CONFIGURE=1`. Starts clean 15/15 |
| Camera | Renders, but paints its own crash page: a `NotImplementedException` in a static constructor. Next target is `ITitleBarClientAdapter` `{ba5e7927-...}`, which our `wineuwp` navigation client refuses and `CoreApplicationViewTitleBar::_TryGetTitleBarClientAdapter` wants |
| Solitaire | **Root cause of the freeze found and proven.** `BootStrap::Startup`'s loop (`0x7dfa4d`) skips the whole frame unless `[bootstrap+0x20a]` (continuous render) or `[+0x208]` (one-shot) is set; `+0x20a` is never set, so exactly one frame ever runs and everything downstream looks frozen. **One byte - `jne`->`jmp` at `0x7dfa64` - makes it run**: bootflow completes, app leaves the splash for `gui/screenfiles/loading/loading`, writes savegames and ticks real time. That byte is a diagnostic; the fix is to deliver the CoreWindow visibility/activation event that sets `+0x20a` (queued closure `0x7d96e0`, created in `0x7d2df4`, invoked via CFG indirect call). **Still not playable**: it holds on the loading screen because `ContentManifestAppComponent::InitiateDownload` fails - the download future completes with an exception *before issuing any request*. msxml6/IXMLHTTPRequest2 was investigated and **cleared** (PlayFab uses it successfully; `probe-http` fetches the manifest with HTTP 200). Next: trampoline the error branch at `0x51c8aa` to print the exception. See Findings-M4 |
| Windows Security | Blank; needs the Defender SecurityHealth service |

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
`PlayFab login successful`. It still stops, one layer deeper, and that layer is now pinned: the
bootflow runs eight items in a fixed order and stalls on the **2nd, `BootflowItemFont`**, whose
completion is gated on a never-fulfilled future loading `gui/fonts/fonts.fontlib`. Items 3, 5 and 7
provably never begin (`ud2` probes with a firing control). The rendering path itself is proven end
to end, which is the part Shapr3D needs.

**Beware ~1-in-3 run variance**: some Solitaire runs stall before `App -- AppState Transition ...
'Sgi::BootflowState'` with no probe applied at all. A probe that "did not fire" in such a run means
nothing - check the run reached the bootflow first.
