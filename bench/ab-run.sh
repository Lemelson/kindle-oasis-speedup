#!/bin/sh
# Mac side: A/B of kppfast modes in one boot, interleaved. Deliberate restarts only: resets Amazon's crash
# counter and kppfast's start-rate log before each restart (otherwise the safety nets would kick in).
cd "$(dirname "$0")"
H=${KINDLE:-kindle}
LOG=${TMPDIR:-/tmp}/ab-run-$$.txt
EXPKEYS=${EXPKEYS:-KPPX11_PIXBUFWIN KPPX11_DRAWPB KPPX11_VERIFY KPPX11_LOG KPPX11_STATS KPPMALLOC KPPIMG KPPMALLOC_LIMITS}
# every KEY that any mode sets is unset in the modes that do not set it (a key missing from EXPKEYS would leak into
# the following modes: batch8, 19.11)
for k in $(echo ${MODES:-} | tr ': ' '\n\n' | sed -n 's/^\(KPP[A-Z0-9_]*\)=.*/\1/p' | sort -u); do case " $EXPKEYS " in *" $k "*) ;; *) EXPKEYS="$EXPKEYS $k" ;; esac; done
ssh $H "mkdir -p /var/local/kpp2" && scp -q keepawake.sh $H:/var/local/kpp2/keepawake.sh
ssh $H "sh /var/local/kpp2/keepawake.sh"
for MODE in ${MODES:-stock all fs x11 stock all}; do
  # MODE = base[:KEY=VAL[:KEY=VAL...]]; experimental keys not given are unset (label: ':' '=' -> '_')
  BASE=${MODE%%:*}; SETS=$(echo "$MODE" | tr ':' ' ' | cut -s -d' ' -f2-); LBL=$(echo "$MODE" | tr ':=,' '___' | sed 's/libkpp//g; s/\.so//g')
  [ ${#LBL} -gt 140 ] && LBL=$(echo "$LBL" | cut -c1-60)_$(echo "$LBL" | cksum | cut -d' ' -f1)   # dir names > 255 bytes fail on the Mac
  CMD="sh /var/local/kppfast/kppfast-ctl.sh $BASE --no-restart >/dev/null 2>&1"
  for k in $EXPKEYS; do CMD="$CMD; sh /var/local/kppfast/kppfast-ctl.sh unset $k --no-restart >/dev/null 2>&1"; done
  for kv in $SETS; do case "$kv" in
      PRE=*) CMD="$CMD; : > /var/local/kppfast/preload.list; for l in $(echo ${kv#PRE=} | tr ',' ' '); do echo /var/local/kppfast/lib/\$l >> /var/local/kppfast/preload.list; done" ;;
      *) CMD="$CMD; sh /var/local/kppfast/kppfast-ctl.sh set $kv --no-restart >/dev/null 2>&1" ;;
    esac; done
  ssh $H "$CMD; grep -c . /var/local/kppfast/preload.list; grep -v '^#' /var/local/kppfast/env | tr '\n' ' '; echo"
  # PRE_RESTART needs no quotes problems: written into a file on the device
  ssh $H 'cat > /var/local/kpp2/prerestart.sh <<X
[ -f /tmp/kppmainapp_crash_counter.txt ] && echo 0 > /tmp/kppmainapp_crash_counter.txt
: > /var/local/kppfast/starts
rm -f /tmp/kppfast.fails /var/local/kppfast/disabled
sh /var/local/kpp2/wake.sh
# another agent may have left the reader as the active app: a restart then ends in "Page cannot be displayed"
[ "\$(lipc-get-prop com.lab126.appmgrd activeApp 2>/dev/null)" = com.lab126.KPPMainApp ] || { lipc-set-prop com.lab126.appmgrd start app://com.lab126.KPPMainApp; sleep 5; }
X
cat > /var/local/kpp2/postrestart.sh <<X
sh /var/local/kpp2/wake.sh
sh /var/local/kpp2/dismiss-error.sh
sh /var/local/kpp2/wake.sh
X'
  KEEP_AWAKE=1 PRE_RESTART=/var/local/kpp2/prerestart.sh POST_RESTART=/var/local/kpp2/postrestart.sh MAXCRASH=99 sh run-bench.sh "$LBL" ${REPS:-2} ${TURNS:-20} > "$LOG" 2>&1; tail -14 "$LOG"
  # abort if the turns did nothing (screen asleep, dialog, wrong coordinates)
  D=$(sed -n 's/^results: //p' "$LOG"); n=$(grep -c '"main_ticks":0,' "$D/turns.jsonl" 2>/dev/null); t=$(grep -c . "$D/turns.jsonl" 2>/dev/null)
  echo "check $LBL: ${n:-?} of ${t:-?} turns without main-thread activity"
  [ -n "$t" ] && [ "$t" -gt 0 ] && [ "$n" -lt $((t/2)) ] || { echo "ABORT: turns not registering"; break; }
done
ssh $H 'mkdir -p /mnt/us/kpp2/crash-reports; for f in /mnt/us/documents/KPPMainAppV2_*; do [ -e "$f" ] && mv "$f" /mnt/us/kpp2/crash-reports/ && echo "moved crash report out of the library: $f"; done; sh /var/local/kpp2/keepawake.sh >/dev/null'
sh "$(dirname "$0")/device-baseline.sh" | sed 's/^/baseline: /'; echo 'config reset to baseline (takes effect at the next KPPMainApp start; device-baseline.sh --restart to apply now)'
