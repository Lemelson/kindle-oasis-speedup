#!/bin/sh
# On-device Library page-turn benchmark for KPPMainApp (Kindle Oasis 3, FW 5.18.2). Runs ON the Kindle.
#   sh /var/local/kpp2/kpp-bench.sh LABEL [REPS] [TURNS]
# Protocol per repetition r=1..REPS:
#   restart KPPMainApp (upstart job), wait until idle, tap the Library tab (recorded as "open"),
#   fwd1: TURNS forward turns from page 1 (pages never shown by this process = "cold" in memory;
#         the on-disk cover cache /mnt/kfxcache/kpp is left as it is, so this is NOT a disk-cold run),
#   back: TURNS back turns (revisits), fwd2: TURNS forward turns again (revisits, "forward warm").
# One JSON line per action -> $OUT/turns.jsonl (fields: see kinject.c). Writes only under /tmp.
LABEL=${1:-run}; REPS=${2:-3}; TURNS=${3:-20}
B=${KB_BIN:-/var/local/kpp2}; KI=$B/kinject
KEYDEV=${KEYDEV:-/dev/input/event3}; TOUCHDEV=${TOUCHDEV:-/dev/input/event8}
KEY_FWD=${KEY_FWD:-104}; KEY_BACK=${KEY_BACK:-109}
LIB_X=${LIB_X:-575}; LIB_Y=${LIB_Y:-283}            # "From Your Library >" on Home (the bottom Library tab ignores injected taps)
JOB=${JOB:-kppmainapp}; IRQ=${IRQ:-}                 # IRQ: substring of a /proc/interrupts line (EPDC), optional
QUIET=${QUIET:-150}; PER=${PER:-5}; GAP_US=${GAP_US:-400000}  # pause between turns after settle
RESTART=${RESTART:-1}
TS=$(date +%Y%m%d-%H%M%S); OUT=/tmp/kpp-bench/$LABEL-$TS; mkdir -p $OUT || exit 1
log() { echo "$(date +%T) $*" | tee -a $OUT/log.txt >&2; }
kpid() { for p in $(pidof KPPMainApp); do [ "$(cat /proc/$p/comm 2>/dev/null)" = KPPMainApp ] && { echo $p; return; }; done; }
xpid() { pidof X 2>/dev/null || pidof Xorg 2>/dev/null; }
ticks() { read l < /proc/$1/stat 2>/dev/null || { echo -1; return; }; set -- ${l##*) }; echo $(( ${12} + ${13} )); }
wait_idle() {  # $1 pid, $2 quiet seconds, $3 timeout seconds: whole-process ticks unchanged for $2 s
  t=0; q=0; last=$(ticks $1)
  while [ $t -lt $3 ]; do usleep 500000; t=$((t+1)); c=$(ticks $1); [ "$c" = -1 ] && return 2
    if [ $((c-last)) -le 1 ]; then q=$((q+1)); else q=0; fi; last=$c; [ $q -ge $(($2*2)) ] && return 0; done; return 1; }
cleanup() { [ "${KEEP_AWAKE:-0}" = 1 ] || lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null; }
trap cleanup EXIT INT TERM
lipc-set-prop com.lab126.powerd preventScreenSaver 1

{ echo "label=$LABEL reps=$REPS turns=$TURNS key_fwd=$KEY_FWD key_back=$KEY_BACK lib=$LIB_X,$LIB_Y quiet=$QUIET per=$PER gap_us=$GAP_US restart=$RESTART"
  uname -a; cat /etc/prettyversion.txt 2>/dev/null; cat /proc/uptime; grep -E "MemFree|MemAvailable|Cached|SwapFree" /proc/meminfo
  cat /sys/class/power_supply/*/capacity 2>/dev/null | head -1 | sed 's/^/battery=/'
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null | sed 's/^/governor=/'
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null | sed 's/^/cur_freq=/'
} > $OUT/meta.txt 2>&1

turn() {  # $1 label, rest = action
  l=$1; shift
  $KI turn -p $KP ${XP:+-x $XP} ${IRQ:+-I $IRQ} -q $QUIET -i $PER -l "$l" -- "$@" >> $OUT/turns.jsonl
}
r=1
while [ $r -le $REPS ]; do
  if [ "$RESTART" = 1 ]; then
    # Amazon's KPPMainAppCrashRecovery (post-stop) counts every exit after 0.1..300 s of uptime as a crash
    # (/tmp/kppmainapp_crash_counter.txt) and opens a recovery screen when the count exceeds 5. Never let it
    # pass 3: if it is already >= MAXCRASH, wait until the running KPPMainApp is >= 305 s old (that exit resets it).
    KP=$(kpid); cnt=$(cat /tmp/kppmainapp_crash_counter.txt 2>/dev/null | tr -dc 0-9); cnt=${cnt:-0}
    if [ -n "$KP" ] && [ "$cnt" -ge ${MAXCRASH:-2} ]; then
      while :; do read up _ < /proc/uptime; read l < /proc/$KP/stat; set -- ${l##*) }; age=$(( ${up%.*} - ${20}/100 ))
        [ $age -ge 305 ] && break; log "crash counter=$cnt, KPPMainApp age ${age}s: waiting for 305 s"; usleep 15000000; done
    fi
    log "rep $r: restart $JOB (crash counter before: $cnt)"
    [ -n "${PRE_RESTART:-}" ] && sh "$PRE_RESTART"   # benchmark only: script that resets counters of deliberate restarts
    stop $JOB >/dev/null 2>&1; usleep 1500000; start $JOB >/dev/null 2>&1
    echo "rep=$r crash_counter_after_restart=$(cat /tmp/kppmainapp_crash_counter.txt 2>/dev/null || echo none)" >> $OUT/meta.txt
    i=0; KP=; while [ -z "$KP" ] && [ $i -lt 60 ]; do usleep 500000; KP=$(kpid); i=$((i+1)); done
    [ -z "$KP" ] && { log "KPPMainApp did not come back"; exit 3; }
    usleep 5000000; wait_idle $KP 3 90 || log "warning: KPPMainApp not idle after start"
    [ -n "${POST_RESTART:-}" ] && sh "$POST_RESTART" >> $OUT/log.txt 2>&1
  fi
  KP=$(kpid); XP=$(xpid)
  echo "rep=$r kpp=$KP x=$XP env: $(tr '\0' '\n' < /proc/$KP/environ | grep -E '^(LD_PRELOAD|DISPLAY|KPP)' | tr '\n' ' ')" >> $OUT/meta.txt
  log "rep $r: kpp=$KP x=$XP, open Library"
  turn "r$r:open:0" tap $TOUCHDEV $LIB_X $LIB_Y 150
  wait_idle $KP 2 30; usleep 1000000
  # verify: the underline under the bottom "Library" tab (fb row 1652, x 910..1030) must be dark
  u=$(dd if=/dev/fb0 bs=1280 skip=1652 count=1 2>/dev/null | od -An -v -tu1 | awk '{for(i=1;i<=NF;i++){n++; if(n>910&&n<=1030&&$i<80)c++}} END{print c+0}')
  echo "rep=$r library_underline=$u" >> $OUT/meta.txt
  if [ "$u" -le 50 ]; then   # a late dialog (crash report) may have eaten the tap: clear it and try once more
    log "rep $r: not in Library (underline=$u), clearing dialogs and retrying"
    [ -n "${POST_RESTART:-}" ] && sh "$POST_RESTART" >> $OUT/log.txt 2>&1
    wait_idle $KP 2 30
    turn "r$r:open:1" tap $TOUCHDEV $LIB_X $LIB_Y 150
    wait_idle $KP 2 30; usleep 1000000
    u=$(dd if=/dev/fb0 bs=1280 skip=1652 count=1 2>/dev/null | od -An -v -tu1 | awk '{for(i=1;i<=NF;i++){n++; if(n>910&&n<=1030&&$i<80)c++}} END{print c+0}')
    echo "rep=$r library_underline_retry=$u" >> $OUT/meta.txt
  fi
  [ "$u" -gt 50 ] || { log "rep $r: NOT IN LIBRARY (underline=$u), abort"; echo NOT_IN_LIBRARY; exit 4; }
  dd if=/dev/fb0 bs=1280 count=1680 2>/dev/null | md5sum | sed "s/ .*/ fb rep$r page1 (stride 1280, since 19.11; older md5s used bs=1264)/" >> $OUT/meta.txt
  echo "r$r:open:0 $(dd if=/dev/fb0 bs=1280 skip=300 count=220 2>/dev/null | md5sum | cut -c1-12)" >> $OUT/pages.txt
  for ph in fwd1 back fwd2; do
    case $ph in back) K=$KEY_BACK;; *) K=$KEY_FWD;; esac
    # after each settled turn: md5 of fb rows 300..519 (first cover row) -> pages.txt; compare.py checks that every turn changed the page
    i=1; while [ $i -le $TURNS ]; do turn "r$r:$ph:$i" key $KEYDEV $K; echo "r$r:$ph:$i $(dd if=/dev/fb0 bs=1280 skip=300 count=220 2>/dev/null | md5sum | cut -c1-12)" >> $OUT/pages.txt; usleep $GAP_US; i=$((i+1)); done
    log "rep $r: $ph done"
  done
  grep -E "VmRSS|VmHWM" /proc/$KP/status | tr -s ' \t' ' ' | sed "s/^/rep=$r /" >> $OUT/meta.txt
  r=$((r+1))
done
grep -E "MemFree|SwapFree" /proc/meminfo >> $OUT/meta.txt; cat /proc/uptime >> $OUT/meta.txt
log "done -> $OUT"; echo $OUT
