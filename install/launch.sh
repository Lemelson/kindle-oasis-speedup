#!/bin/sh
# kppfast launch wrapper for Amazon's KPPMainApp (Kindle Oasis 3, FW 5.18.2). Installed as
# /var/local/kppfast/launch.sh and exec'd by the "script" section of /etc/upstart/kppmainapp.conf in place of
# "exec /app/bin/KPPMainApp". It always ends in "exec /app/bin/KPPMainApp" (same pid for upstart), with or without
# LD_PRELOAD. No "set -e": every failure path falls through to the stock launch.
#
# Stock launch (no LD_PRELOAD) when any of these holds:
#   /mnt/us/DISABLE_KPPFAST exists (kill switch, reachable over USB)  |  /var/local/kppfast/disabled exists
#   fails >= 2 (two preloaded starts did not reach 60 s of uptime)     |  >= 4 preloaded starts within 30 min
#   preload.list empty / a listed library missing or not matching lib/SHA256SUMS
# Fail counter (/tmp/kppfast.fails, per boot): fails+1 is written BEFORE exec; a background watcher resets it to 0 once the same process has
# been alive 60 s with its LIPC service up (no LIPC after 180 s = hang -> kill -9 = failure). Amazon's crash-loop detector (KPPMainAppCrashRecovery) only shows its recovery UI after the
# 6th exit within 300 s of a start, so the fallback to stock (3rd start) always comes first.
D=/var/local/kppfast
LOG=$D/launch.log
F=/tmp/kppfast.fails   # tmpfs: per boot (a quick reboot is not a failure); cross-boot loops: the starts rate limit
APP=/app/bin/KPPMainApp
log() { echo "$(date '+%F %T') [launch $$] $*" >> $LOG 2>/dev/null; }
stock() { log "STOCK launch: $*"; unset LD_PRELOAD; exec $APP; }

# keep the log small (ext3 /var/local)
[ -f $LOG ] && [ "$(wc -c < $LOG 2>/dev/null)" -gt 65536 ] 2>/dev/null && { tail -n 200 $LOG > $LOG.tmp && mv -f $LOG.tmp $LOG; }

[ -e /mnt/us/DISABLE_KPPFAST ] && stock "kill switch /mnt/us/DISABLE_KPPFAST"
[ -e $D/disabled ] && stock "disabled: $(head -c 200 $D/disabled 2>/dev/null)"

fails=$(cat $F 2>/dev/null); case "$fails" in ''|*[!0-9]*) fails=0 ;; esac
if [ "$fails" -ge 2 ]; then
  echo "$(date '+%F %T') $fails preloaded starts in a row died before 60 s" > $D/disabled; sync
  stock "fail-safe: fails=$fails -> wrote $D/disabled"
fi

# library list: absolute paths under $D/lib, one per line, '#' comments; each must match lib/SHA256SUMS
PRE=""
if [ -f $D/preload.list ]; then
  while read -r L _; do
    case "$L" in ''|'#'*) continue ;; esac
    case "$L" in $D/lib/*.so) ;; *) stock "rejected preload entry '$L' (must be $D/lib/*.so)" ;; esac
    [ -f "$L" ] || stock "missing library $L"
    want=$(awk -v f="${L##*/}" '$2 == f || $2 == "./" f { print $1 }' $D/lib/SHA256SUMS 2>/dev/null)
    have=$(sha256sum "$L" 2>/dev/null | awk '{ print $1 }')
    [ -n "$want" ] && [ "$want" = "$have" ] || stock "SHA-256 mismatch for $L"
    PRE="${PRE:+$PRE:}$L"
  done < $D/preload.list
fi
[ -n "$PRE" ] || stock "empty preload.list"

# rate limit: preloaded starts in the last 30 min (late crashes reset 'fails' but still show up here)
now=$(date +%s)
n=$(awk -v t=$((now - 1800)) '$1 >= t' $D/starts 2>/dev/null | wc -l)
if [ "$n" -ge 4 ]; then
  echo "$(date '+%F %T') $n preloaded starts within 30 min" > $D/disabled; sync
  stock "fail-safe: $n preloaded starts within 30 min -> wrote $D/disabled"
fi
{ tail -n 9 $D/starts 2>/dev/null; echo $now; } > $D/starts.tmp && mv -f $D/starts.tmp $D/starts

# env switches: only KPP*=<safe chars> lines are exported (the file is never sourced)
ENVS=""
if [ -f $D/env ]; then
  while read -r L; do
    case "$L" in ''|'#'*) continue ;; esac
    k=${L%%=*}; v=${L#*=}
    case "$k" in KPP[A-Z0-9_]*) ;; *) log "ignored env line '$L'"; continue ;; esac
    case "$k$v" in *[!A-Za-z0-9_./:-]*) log "ignored env line '$L'"; continue ;; esac
    export "$k=$v"; ENVS="$ENVS $k=$v"
  done < $D/env
fi

echo $((fails + 1)) > $F.tmp && mv -f $F.tmp $F
P=$$
# watcher: healthy = the same process is alive after 60 s AND its LIPC service com.lab126.KPPMainApp answers
# -> fails=0. Still no LIPC answer after READY_S (default 180 s; a hang in a library constructor) -> kill -9, so a
# hang counts as a failed start like a crash (upstart respawns; 2 failures -> stock).
RS=$(cat $D/ready_timeout 2>/dev/null); case "$RS" in ''|*[!0-9]*) RS=180 ;; esac
( alive() { [ -d /proc/$P ] && [ "$(awk '{ print $3 }' /proc/$P/stat 2>/dev/null)" != Z ] &&
            tr '\0' ' ' < /proc/$P/cmdline 2>/dev/null | grep -q '/app/bin/KPPMainApp'; }
  probe() { if command -v timeout >/dev/null 2>&1; then timeout 10 lipc-probe com.lab126.KPPMainApp; else lipc-probe com.lab126.KPPMainApp; fi; }
  t=0; ready=0
  while [ $t -lt $RS ]; do
    sleep 10; t=$((t + 10)); alive || exit 0
    [ $ready = 0 ] && probe >/dev/null 2>&1 && { ready=1; log "pid $P: LIPC ready after ~${t} s"; }
    if [ $ready = 1 ] && [ $t -ge 60 ]; then
      echo 0 > $F.tmp && mv -f $F.tmp $F
      log "healthy: pid $P alive for $t s with LIPC up, fails reset"; exit 0
    fi
  done
  alive && { log "pid $P: no LIPC answer after $RS s -> kill -9 (counts as a failed start)"; kill -9 $P; }
) < /dev/null > /dev/null 2>&1 &
log "PRELOAD launch (fails now $((fails + 1))): LD_PRELOAD=$PRE$ENVS"
export LD_PRELOAD="$PRE"
exec $APP
