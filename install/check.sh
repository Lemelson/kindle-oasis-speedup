#!/bin/sh
# kppfast check: is it installed, intact, and is the running KPPMainApp preloaded and healthy?
# Exit 0 = installed + healthy (preloaded, or stock on purpose with a stated reason), 1 = problem.
# Kindle Sync runs this after the reboot (4 min uptime) and reports the output.
HERE="$(cd "$(dirname "$0")" && pwd)"
KF_ROLE=check
. /var/local/kppfast/kppfast-lib.sh 2>/dev/null || . "$HERE/kppfast-lib.sh" || { echo "kppfast-lib.sh not found"; exit 1; }
rc=0
bad() { echo "PROBLEM: $*"; rc=1; }
echo "firmware: $(firmware_ok && echo ok || echo UNEXPECTED)"
ST="$(conf_state)"; echo "kppmainapp.conf: $ST"
[ "$ST" = kppfast ] || bad "job file is $ST"
[ -f "$KF_DIR/launch.sh" ] || bad "missing $KF_DIR/launch.sh"
( cd "$KF_DIR/lib" 2>/dev/null && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) && echo "libraries: verified ($(cd $KF_DIR/lib && ls *.so | tr '\n' ' '))" || bad "libraries do not verify"
echo "preload.list: $(grep -v '^#' $KF_DIR/preload.list 2>/dev/null | tr '\n' ' ')"
echo "env: $(grep -v '^#' $KF_DIR/env 2>/dev/null | tr '\n' ' ')"
echo "fails=$(cat /tmp/kppfast.fails 2>/dev/null) disabled=$(cat $KF_DIR/disabled 2>/dev/null || echo no) kill-switch=$( [ -e /mnt/us/DISABLE_KPPFAST ] && echo ON || echo off)"
echo "amazon crash counter: $(cat $KPP_CRASH_COUNTER 2>/dev/null || echo none)   root mount: $(root_mount_mode)"
P="$(kpp_pid)"
if [ -z "$P" ]; then bad "KPPMainApp not running"
else
  # launch.sh logs its own pid, which is KPPMainApp's pid after exec; /proc/<pid>/maps confirms when readable
  LL="$(grep "\[launch $P\] \(PRELOAD\|STOCK\)" $KF_DIR/launch.log 2>/dev/null | tail -n 1)"
  PRE="$(echo "$LL" | sed -n 's/.*PRELOAD launch.*LD_PRELOAD=\([^ ]*\).*/\1/p')"
  MAPS="$(grep -o '/var/local/kppfast/lib/[^ ]*\.so' /proc/$P/maps 2>/dev/null | sort -u | tr '\n' ' ')"
  echo "KPPMainApp pid $P, age $(kpp_age) s, launched with LD_PRELOAD=[$PRE], mapped: [${MAPS:-not readable or none}]"
  [ -n "$LL" ] || echo "note: no launch.log line for pid $P (started before the install, or log rotated)"
  if [ -n "$PRE" ] && head -n 1 /proc/$P/maps >/dev/null 2>&1 && [ -z "$MAPS" ]; then bad "launched with LD_PRELOAD but no kppfast library mapped"; fi
  if [ -z "$PRE" ]; then
    if [ -e /mnt/us/DISABLE_KPPFAST ] || [ -f $KF_DIR/disabled ] || ! grep -q . $KF_DIR/preload.list 2>/dev/null; then
      echo "stock launch on purpose"
    else bad "stock launch without a reason (see $KF_DIR/launch.log)"; fi
  fi
fi
echo "--- last launch.log lines"; tail -n 5 $KF_DIR/launch.log 2>/dev/null
[ $rc = 0 ] && echo "RESULT: OK" || echo "RESULT: PROBLEM"
exit $rc
