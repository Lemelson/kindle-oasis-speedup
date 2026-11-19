#!/bin/sh
# Mac side: exclusive use of the real Kindle between parallel agents (only one may restart KPPMainApp / run benches).
#   device-lock.sh acquire OWNER [WAIT_MIN]   -> waits (default 120 min) for the lock, then holds it
#   device-lock.sh release OWNER
#   device-lock.sh status
# Lock = directory (atomic mkdir). A lock older than 45 min without refresh is considered stale and taken over.
L=${KPP_LOCK_DIR:-$(cd "$(dirname "$0")" && pwd)/.device-lock}
cmd=$1; who=${2:-?}; wait=${3:-120}
case "$cmd" in
  acquire)
    end=$(( $(date +%s) + wait * 60 ))
    Q="$L.queue"; mkdir -p "$Q"; :
    me=$(ls "$Q" 2>/dev/null | grep -- "-$who\$" | head -1); [ -n "$me" ] || { me="$(date +%s)-$who"; : > "$Q/$me"; }
    trap 'rm -f "$Q/$me"' EXIT INT TERM
    while :; do
      touch "$Q/$me"; find "$Q" -type f -mmin +2 ! -name "$me" -delete 2>/dev/null   # FIFO; dead waiters drop out after 2 min
      first=$(ls "$Q" | sort | head -1)
      if [ "$first" = "$me" ] && mkdir "$L" 2>/dev/null; then rm -f "$Q/$me"; trap - EXIT INT TERM; echo "$who $(date +%s)" > "$L/owner"; echo "locked by $who"
        H=$(dirname "$0"); sh "$H/device-health.sh" > "$L/health-before.txt" 2>&1; hr=$?
        cat "$L/health-before.txt"
        if [ $hr = 1 ]; then echo "REFUSED: the Kindle is UNHEALTHY before your session (see above). Do not experiment; tell the main session."; rm -rf "$L"; exit 3; fi
        exit 0; fi
      # lock free but somebody else is first in the queue (or the winner has not written its owner file yet): wait.
      # Without this the empty owner looked "stale" and the loop deleted the winner's fresh lock (19.11, lib/reader2).
      [ -s "$L/owner" ] || { sleep 3; [ -d "$L" ] && [ ! -s "$L/owner" ] && [ -n "$(find "$L" -maxdepth 0 -mmin +2 2>/dev/null)" ] && { echo "lock dir without owner for > 2 min, removing"; rm -rf "$L"; }; [ $(date +%s) -ge $end ] && { echo "TIMEOUT: queue"; exit 1; }; continue; }
      o=$(cat "$L/owner" 2>/dev/null); t=${o##* }
      case "$t" in ''|*[!0-9]*) t=0 ;; esac
      if [ $(( $(date +%s) - t )) -gt 2700 ]; then echo "stale lock ($o), taking over"; rm -rf "$L"; continue; fi
      [ "${o%% *}" = "$who" ] && { echo "$who $(date +%s)" > "$L/owner"; echo "already held by $who (refreshed)"; exit 0; }
      [ $(date +%s) -ge $end ] && { echo "TIMEOUT: lock held by $o"; exit 1; }
      sleep 30
    done ;;
  refresh) o=$(cat "$L/owner" 2>/dev/null); [ "${o%% *}" = "$who" ] && echo "$who $(date +%s)" > "$L/owner" && echo refreshed ;;
  release) o=$(cat "$L/owner" 2>/dev/null); [ "${o%% *}" = "$who" ] || { echo "not held by $who ($o)"; exit 1; }
    sh "$(dirname "$0")/device-health.sh"; hr=$?
    [ $hr = 1 ] && echo "WARNING: you are leaving the Kindle UNHEALTHY. Fix it (device-health.sh --heal reboots a broken GUI) or report it to the main session NOW."
    rm -rf "$L"; echo released; exit $hr ;;
  status) [ -d "$L" ] && echo "held: $(cat $L/owner) ($(( $(date +%s) - $(cut -d' ' -f2 $L/owner) )) s ago)" || echo free
          ls "$L.queue" 2>/dev/null | sort | sed 's/^/waiting: /' ;;
  *) sed -n '2,6p' "$0"; exit 2 ;;
esac
