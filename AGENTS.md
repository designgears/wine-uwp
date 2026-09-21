# Working on wine-uwp

This repo is Microsoft's Calculator, but the work here is `tools/wine-uwp` and `docs/LinuxPort`:
running **packaged UWP apps we have no source for** under a patched Wine. The target is Shapr3D.
Calculator, Feedback Hub, Media Player and Camera are test subjects, in that order of usefulness -
Calculator because it is small and byte-comparable, the others because they are the same .NET
Native + WinUI 2 shape Shapr3D is.

`docs/LinuxPort/Plan.md` has the architecture. `docs/LinuxPort/Findings-M*.md` are the running
log - **append findings there**, including the ones that did not work. This file is the part that
generalises: how to investigate, and the mistakes that have already cost time.

## Ground rules

- **Run on Xvfb `:98`, never the user's `:0`.** `DISPLAY=:98 tools/wine-uwp/run.sh`.
- **The Wine tree (`~/src/wine`) is shared with another session and is deliberately uncommitted.**
  Check `git -C ~/src/wine status` before touching anything. Files that are not yours include
  `dlls/kernelbase/*`, `dlls/ntdll/{loader,rtl,sync,signal_x86_64,threadpool}.c`, `dlls/win32u`,
  `dlls/user32`, `dlls/combase`, `dlls/d3d11`, `dlls/windows.graphics`, `dlls/wintypes`, `server/*`,
  `include/*`. Commit only files in *this* repo.
- **Never push.** Commit freely; pushing is the user's call.
- **Byte patches go in `~/uwp-bin/win11/...`**; `run.sh` re-copies into the prefix. Always keep a
  backup and verify the restore with `cmp` before moving on. Leaving one behind silently poisons
  every later run - it has happened.
- Commit messages end with the attribution line the harness gives you.

## The regression protocol - do not skip it

Every change to Wine or the host gets checked against apps that already work, *before* you believe
a result:

```
# Calculator must stay byte-identical to the saved baseline
DISPLAY=:98 UWP_TIMEOUT=80 UWP_LOG=/tmp/c.log tools/wine-uwp/run.sh &  sleep 48
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

| Question | Tool |
| --- | --- |
| What is this IID, and what are its methods? | `tools/wine-uwp/winmd.py <winmd> --guid <iid>` (public interfaces; prints slots from 6) |
| Demangle an MSVC symbol | `llvm-undname` |
| Symbols for a Microsoft binary | read the RSDS record from the PE debug directory, `curl` it from `msdl.microsoft.com/download/symbols/<name>/<sig><age>/<name>` ([docs](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/microsoft-public-symbols)), then `llvm-pdbutil dump --publics` into a `rva name` `.syms` file next to it in `~/uwp-bin/pdb`. `tools/wine-uwp/pdbsyms.py` does this but needs `pefile`, which is **not installed** - parse the debug directory by hand (~20 lines) or install it |
| Which module is this address in? | `tools/wine-uwp/addr2mod.py <log-with-+loaddll> <addr>` - **native modules only**, see below; for a Wine builtin use `/proc/<pid>/maps` |
| Which of N call sites returned this HRESULT? | unique-HRESULT tagging, below |
| Did this code path run at all? | patch `0f 0b` (`ud2`) over its first two bytes, below |
| What did managed code throw, and where? | `WINE_UWP_TRACE_ORIGINATE=1` + the chunked `trace_language_exception` in `dlls/combase/roerror.c` |
| How many real `CoreApplicationView`s exist, on which threads? | `WINE_UWP_REPORT_VIEWS=1` (read-only census) |
| What is the visual tree doing? | `WINEDEBUG=+dcomp`; `present_desktop_hwnd frame: N ops` - 0 ops means nothing to draw |
| What is every thread actually doing? | `WINE_UWP_DUMP_STACKS=<seconds>` - suspends and unwinds every thread, prints `STACK tid N frame M rip=`. Works on an *idle* app, not just a hung one. Resolve against `/proc/<pid>/maps` from the same run |
| Method count of a private interface | parse the MIDL proxy/stub DLL, below |

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

**Patch and restore *every* copy.** `run.sh` copies `~/uwp-bin/win11/...` into the prefix, so a
patch applied to the source is live in `~/.wine-uwp/drive_c/windows/system32/` too - restoring only
the source leaves the probe in place for every later run. `cmp` both against the backup afterwards.


To answer "does this basic block execute", overwrite its first two bytes with `0f 0b`. A hit shows
up as `err:seh:NtRaiseException Unhandled exception code 1007`. The *address* in that line is the
Wine-side rethrow and is the same for every probe - only the presence of the line means anything.

**Always run a control probe** (a `ud2` somewhere that must execute) before trusting a negative.
Three negatives in a row usually means the patched file is not the file being loaded.

### Reading a private interface

For interfaces that are in no `.winmd` and unnamed in `HKCR\Interface`:

1. `HKCR\Interface\{iid}` in the Windows SOFTWARE hive (`tools/wine-uwp/fasthive.py`) often has the
   name even when the metadata does not.
2. Find which DLLs contain the IID bytes. A **proxy/stub** DLL gives the exact method count: find a
   pointer to the IID (that is `CInterfaceStubHeader::piid`), read `DispatchTableCount` at +0x10.
3. Best of all: find the **shipping implementation's vtable** and let its PDB name every slot. That
   is how `{9d47a108}`'s 13 methods were recovered - the 19-entry `MediaPlayerImpl` vtable in
   `Windows.Media.Playback.MediaPlayer.dll`. Do this before writing a single stub.
4. Failing all that, implement the interface with one traced thunk per slot and let the app tell you
   which methods it calls.

## Mistakes already made - do not repeat them

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

**Check who owns a file before "fixing" it.** `RtlGetDeviceFamilyInfoEnum` has a ternary that reads
inverted; it is correct, because `get_package_env` returns an `NTSTATUS` and 0 means success. It is
also in another session's file.

**Do not add fields to a struct whose prefix is shared.** Everything in `dlls/dcomp/composition.c`
starts with `struct comp_object`'s three fields and reuses its AddRef/Release. A field added there
landed on `kf_animation`'s `target[]` and made Release iterate 0x664f garbage entries - the fault
report showed `rdx 007300660066004f`, `"Offs"` from `"Offset"`. Put new state in its own allocation.

**Do not blame the shared tree until you have dated proof, and do not declare an app broken
without a screenshot.** Media Player rendered and navigated, then a later run failed to start. I
wrote both of the following into this file, and both were wrong:

- *"ntdll and kernelbase were rebuilt minutes ago by the other session."* I had run
  `ls -lt --time-style=+%H:%M`, which prints **no date**, saw `03:43`, and assumed today. With
  `--time-style='+%m-%d %H:%M'` those DLLs are from **09-19**. The only file rebuilt that day was my
  own `windows.media.playback.mediaplayer.dll`. Always print the date; also check
  `~/.wine-uwp/.update-timestamp` for a wineboot and `cmp` the byte-patched prefix DLLs.
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
   (`tools/wine-uwp/probes/probe-load.cpp` reports per-DLL load times).

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
- A Wine DLL can have a **copy in the prefix** (`~/.wine-uwp/drive_c/windows/system32/`) shadowing
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

## Current state (2026-09-20)

| App | State |
| --- | --- |
| Calculator | Fully working; the byte-identical regression baseline |
| Feedback Hub | Fully working and interactive (~822 colours) |
| Media Player | **Plays audio from its own UI** - Open file(s) -> pick -> Play decodes WAV and MP3 through MF to `waveOut`. Needs `WINE_UWP_DRIVE_VIEW=run WINE_UWP_DRIVE_FIRST_VIEW=1 WINE_UWP_MANUAL_CONFIGURE=1`. Starts clean 15/15 |
| Camera | Renders, but paints its own crash page: a `NotImplementedException` in a static constructor. Next target is `ITitleBarClientAdapter` `{ba5e7927-...}`, which our `wineuwp` navigation client refuses and `CoreApplicationViewTitleBar::_TryGetTitleBarClientAdapter` wants |
| Solitaire | **A native C++ engine (`Sgi::`), not a XAML app** - so its never calling `Window.put_Content`/`Activate` (ud2-proved) is expected, not the bug. XAL and XSAPI now initialise. The app is alive, responsive (closes cleanly in 2 s) and running a 60 Hz dispatcher loop with a real 1280x800 window, but never reaches its own `Startup START` and draws nothing. Closest proxy we have to Shapr3D's rendering path |
| Windows Security | Blank; needs the Defender SecurityHealth service |

The three Media Player switches are opt-in only because the other apps have not been tested against
them. **Testing that, and making them the default if it holds, is the next job** - they change the
view/activation ordering for everyone, which is exactly the sort of thing Shapr3D will also depend
on. One data point so far: `WINE_UWP_MANUAL_CONFIGURE` is **load-bearing for Media Player** and
cannot simply be dropped - without it the app never paints (8 runs, 8 no-paint), because
`broadcast_configure_core_window` returns before both the direct call and the APC broadcast.

The `ConfigureCoreWindow` hang that used to lose one run in three is **fixed** - it was our own
APC landing on Wine's RPC server thread, not anything in XAML. See Findings-M4.md.
