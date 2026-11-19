#!/bin/sh
# Device side: keep the Kindle out of screensaver while the Mac session works on it, smartly.
#   keepawake.sh [anything]  - "poke": counts as activity, starts the watcher if needed
#   keepawake.sh off         - release now
#   keepawake.sh status
# The watcher checks every 10 s. Activity = an SSH session is open (dropbear child), a benchmark/injector is
# running, or a poke. While active, preventScreenSaver=1. After IDLE_MIN (default 30) minutes without activity it
# sets preventScreenSaver=0 (normal Amazon sleep settings) and exits. Nothing persists across a reboot.
P=/var/run/kpp2-keepawake.pid; S=/var/run/kpp2-keepawake.last
IDLE_MIN=$(cat /var/local/kpp2/keepawake-idle-min 2>/dev/null); case "$IDLE_MIN" in ''|*[!0-9]*) IDLE_MIN=30 ;; esac
alive() { [ -f $P ] && kill -0 "$(cat $P)" 2>/dev/null; }

busy() {
  for d in /proc/[0-9]*; do
    c=$(tr '\0' ' ' < $d/cmdline 2>/dev/null) || continue
    case "$c" in
      *dropbear*" -2 "*) return 0 ;;                       # an open SSH session (master has no -2)
      *kpp-bench.sh*|*kinject*|*run-bench*) return 0 ;;
    esac
  done
  return 1
}

watch() {
  echo $$ > $P
  lipc-set-prop com.lab126.powerd preventScreenSaver 1
  while :; do
    now=$(date +%s)
    busy && echo $now > $S
    last=$(cat $S 2>/dev/null || echo $now)
    if [ $((now - last)) -ge $((IDLE_MIN * 60)) ]; then
      lipc-set-prop com.lab126.powerd preventScreenSaver 0
      rm -f $P $S; exit 0
    fi
    # someone else (a test cleanup) may have cleared it while we are still active
    [ "$(lipc-get-prop com.lab126.powerd preventScreenSaver 2>/dev/null)" = 1 ] || lipc-set-prop com.lab126.powerd preventScreenSaver 1
    sleep 10
  done
}

case "$1" in
  off) alive && kill "$(cat $P)"; rm -f $P $S; lipc-set-prop com.lab126.powerd preventScreenSaver 0; echo "keepawake: off"; exit 0 ;;
  status) if alive; then l=$(cat $S); echo "keepawake: on, last activity $(( $(date +%s) - l )) s ago, releases after $IDLE_MIN min idle"; else echo "keepawake: off"; fi; exit 0 ;;
  __watch) watch ;;
esac
date +%s > $S
lipc-set-prop com.lab126.powerd preventScreenSaver 1
if ! alive; then
  if command -v setsid >/dev/null 2>&1; then setsid sh "$0" __watch </dev/null >/dev/null 2>&1 &
  else sh "$0" __watch </dev/null >/dev/null 2>&1 & fi
  sleep 1
fi
echo "keepawake: on while working, normal sleep after $IDLE_MIN min without activity"
