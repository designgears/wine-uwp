#!/bin/bash
# The regression protocol from AGENTS.md, polled instead of slept: each app is checked once a second and
# the next one starts as soon as it passes (or its time limit runs out).
#
#   TESTDISPLAY=:97 ./regress.sh [calc] [fh] [mp] [sol] [SOLRUNS=3]     default: all four
#
# calc  Calculator (Store 11.2607) identical to docs/baselines/calc-regress.png (AE 0)
# fh    Feedback Hub: colour count settles at ~899, no fail-fasts
# mp    Media Player: 2835 colours on prefix-store (11-17)
# sol   Solitaire 4.27: its own log (LocalState/Logs) enters the main menu screen, >= 1000 colours, no CRITICAL ERROR; SOLRUNS runs
# kb    Calculator keyboard: 12+3 Enter, Backspace 7, Escape, each identical to docs/baselines/calc-kb-*.png. Not in the
#       default set of four only for time; run it after any change to input, Windows.UI or its byte patches (Enter,
#       Backspace and Escape all died once while calc, fh, mp and sol still passed). KB_BASELINE=1 rewrites the baselines.
cd "$(dirname "$0")"; . ./env.sh; export PATH="$WINE_BUILD:$WINE_BUILD/server:$PATH"   # server/: a build copy has only wine at its top
D=${TESTDISPLAY:-:98}
apps=${*:-calc fh mp sol}

shot() { DISPLAY=$D xwd -root -silent > /tmp/rg.xwd 2>/dev/null && magick xwd:/tmp/rg.xwd "$1"; }
start() { wineserver -k 2>/dev/null; wineserver -w; (env DISPLAY=$D UWP_TIMEOUT=120 "$@" ./run.sh >/dev/null 2>&1 &); }
stop() { wineserver -k 2>/dev/null; wineserver -w; }
# poll <limit s> <check command>: runs the check each second until it succeeds
poll() { local limit=$1 t0=$SECONDS; shift; while [ $((SECONDS - t0)) -lt "$limit" ]; do sleep 1; "$@" && return 0; done; return 1; }
colours() { shot /tmp/rg.png && identify -format %k /tmp/rg.png; }
# the colour count has been the same, and at least $1, for three samples in a row
stable_since=0; last=-1
settled() { local k; k=$(colours); if [ "$k" = "$last" ] && [ "$k" -ge "$1" ]; then stable_since=$((stable_since+1)); else stable_since=0; fi; last=$k; [ $stable_since -ge 2 ]; }

for app in $apps; do
  t0=$SECONDS
  case $app in
  calc)
    start UWP_LOG=/tmp/rg-calc.log
    ae() { shot /tmp/rg-calc.png && [ "$(magick compare -metric AE /tmp/rg-calc.png docs/baselines/calc-regress.png null: 2>&1 | cut -d' ' -f1)" = 0 ]; }
    poll 60 ae; echo "calc AE $(magick compare -metric AE /tmp/rg-calc.png docs/baselines/calc-regress.png null: 2>&1) ($((SECONDS-t0)) s)";;
  fh)
    start UWP_STAGE=windowsfeedbackhub UWP_LOG=/tmp/rg-fh.log; stable_since=0; last=-1
    poll 70 settled 800; cp /tmp/rg.png /tmp/rg-fh.png
    echo "fh $last colours, failfast $(grep -ac 'RoFailFast\|FailFast' /tmp/rg-fh.log) ($((SECONDS-t0)) s)";;
  mp)
    start UWP_STAGE=media-player WINE_UWP_DRIVE_VIEW=run WINE_UWP_DRIVE_FIRST_VIEW=1 WINE_UWP_MANUAL_CONFIGURE=1 UWP_LOG=/tmp/rg-mp.log
    stable_since=0; last=-1; poll 70 settled 2000; cp /tmp/rg.png /tmp/rg-mp.png
    echo "mp $last colours ($((SECONDS-t0)) s)";;
  sol)
    # 4.27 logs to its LocalState, not stderr; each start rotates that file to Solitaire_old.log
    sol_log="$WINEPREFIX/drive_c/users/$USER/AppData/Local/Packages/Microsoft.MicrosoftSolitaireCollection_8wekyb3d8bbwe/LocalState/Logs/Solitaire.log"
    bad=0
    for r in $(seq ${SOLRUNS:-3}); do
      t1=$SECONDS; rm -f "$sol_log"; start UWP_STAGE=microsoftsolitairecollection UWP_LOG=/tmp/rg-sol-$r.log
      fe() { grep -aq "EnterScreen \[gui/screenfiles/mainmenu" "$sol_log" 2>/dev/null && [ "$(colours)" -ge 1000 ]; }
      poll ${SOLLIMIT:-45} fe; ok=$?
      shot /tmp/rg.png; cp "$sol_log" /tmp/rg-sol-$r.app.log 2>/dev/null
      c=$(grep -ac 'CRITICAL ERROR' /tmp/rg-sol-$r.log /tmp/rg-sol-$r.app.log 2>/dev/null | awk -F: '{n+=$2} END {print n+0}')
      [ $ok = 0 ] && [ "$c" = 0 ] || bad=$((bad+1))
      echo "sol run $r: $( [ $ok = 0 ] && echo main menu || echo NOT reached), $(identify -format %k /tmp/rg.png) colours, $c critical ($((SECONDS-t1)) s)"
      stop
    done
    echo "sol: $bad/${SOLRUNS:-3} bad";;
  kb)
    start UWP_LOG=/tmp/rg-kb.log
    ae() { shot /tmp/rg-calc.png && [ "$(magick compare -metric AE /tmp/rg-calc.png docs/baselines/calc-regress.png null: 2>&1 | cut -d' ' -f1)" = 0 ]; }
    if ! poll 60 ae; then echo "kb: Calculator did not come up"; stop; continue; fi
    W=$(DISPLAY=$D xdotool search --onlyvisible --name Calculator | tail -1); DISPLAY=$D xdotool windowactivate $W; sleep 0.5
    res=""
    for step in enter bs esc; do
      case $step in
      enter) DISPLAY=$D xdotool type --delay 150 "12+3"; DISPLAY=$D xdotool key --delay 150 Return;;
      bs)    DISPLAY=$D xdotool key --delay 150 BackSpace; DISPLAY=$D xdotool type --delay 150 "7";;
      esc)   DISPLAY=$D xdotool key --delay 150 Escape;;
      esac
      sleep 1; shot /tmp/rg-kb-$step.png
      if [ -n "${KB_BASELINE:-}" ]; then cp /tmp/rg-kb-$step.png docs/baselines/calc-kb-$step.png; res="$res $step:baseline"
      else res="$res $step:$(magick compare -metric AE /tmp/rg-kb-$step.png docs/baselines/calc-kb-$step.png null: 2>&1 | cut -d' ' -f1)"; fi
    done
    echo "kb AE$res (0 = identical; Enter shows 15, Backspace 7 then 7, Escape 0) ($((SECONDS-t0)) s)";;
  esac
  stop
done
