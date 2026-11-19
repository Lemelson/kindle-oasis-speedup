#!/bin/sh
# kppfast: shared definitions for install.sh / rollback.sh / check.sh / kppfast-ctl.sh (busybox sh, FW 5.18.2).
# The ONLY rootfs change is one line added before "exec /app/bin/KPPMainApp" in /etc/upstart/kppmainapp.conf:
KF_LINE='[ -e /mnt/us/DISABLE_KPPFAST ] || [ ! -f /var/local/kppfast/launch.sh ] || exec /bin/sh /var/local/kppfast/launch.sh # kppfast'
KF_DIR=/var/local/kppfast
KF_CONF=/etc/upstart/kppmainapp.conf
NRR=/mnt/us/kppfast-logs
KF_LOG=$NRR/kppfast.log
KF_BACKUPS=$NRR/backups
KF_STOCK_COPY=$KF_DIR/kppmainapp.conf.stock
CONF_STOCK_SHA256=ecf2f590e9f6222bb87f6697816c43c482967751aaa841ba2e42cdabe4b8625d
CONF_KF_SHA256=5f926c6ccfa3839cee372cf91c0109642b6908ffb3c72fe7d458dadba45a1f05
EXPECTED_FIRMWARE_BUILD="025-juno_1802_stinger_zelda-443455"
V32_GUARD_STATE=/var/local/nativepatch-v32/guard.state   # author's separate reader patch; absent elsewhere (check passes)
KPP_CRASH_COUNTER=/tmp/kppmainapp_crash_counter.txt
TS="${TS:-$(date +%Y%m%d-%H%M%S)}"

kf_log() { _l="$(date '+%F %T') [kppfast ${KF_ROLE:-?}] $*"; echo "$_l"; [ -d "$NRR" ] && echo "$_l" >> "$KF_LOG" 2>/dev/null; }
kf_fail() { kf_log "ERROR: $*"; exit 1; }
sha256_of() { sha256sum "$1" 2>/dev/null | awk '{ print $1 }'; }
root_mount_mode() { awk '$2 == "/" { o = $4 } END { split(o, a, ","); print a[1] }' /proc/mounts 2>/dev/null; }
firmware_ok() { [ -r /etc/version.txt ] && grep -F -q "$EXPECTED_FIRMWARE_BUILD" /etc/version.txt; }
free_kb() { df -P -k "$1" 2>/dev/null | awk 'NR == 2 { print $4 }'; }
# conf_state: stock | kppfast | unknown
conf_state() {
  _s="$(sha256_of "$KF_CONF")"
  [ "$_s" = "$CONF_STOCK_SHA256" ] && { echo stock; return; }
  [ "$_s" = "$CONF_KF_SHA256" ] && { echo kppfast; return; }
  echo unknown
}
# make_kf_conf <stock conf> <out>: insert KF_LINE before the (single) exec line of the script section
make_kf_conf() {
  [ "$(grep -c '^exec /app/bin/KPPMainApp$' "$1")" = 1 ] || return 1
  awk -v L="$KF_LINE" '$0 == "exec /app/bin/KPPMainApp" { print L } { print }' "$1" > "$2"
}
kpp_pid() {
  for _p in $(pidof KPPMainApp KPPMainAppV2 2>/dev/null); do
    [ "$(awk '{ print $3 }' "/proc/$_p/stat" 2>/dev/null)" = Z ] || { echo "$_p"; return 0; }
  done; return 1
}
kpp_age() { # seconds since the running KPPMainApp started (or empty)
  _p="$(kpp_pid)" || return 1
  _st=$(awk '{ print $22 }' /proc/$_p/stat 2>/dev/null); _hz=100
  awk -v s="$_st" -v hz=$_hz '{ printf "%d\n", $1 - s / hz }' /proc/uptime
}
# before any deliberate KPPMainApp restart: Amazon counts exits within 300 s of a start; >5 -> recovery UI.
# Keep a margin: with counter >= 2 wait until the running process is >= 305 s old (max 6 min).
crash_counter_guard() {
  _c="$(cat $KPP_CRASH_COUNTER 2>/dev/null)"; case "$_c" in ''|*[!0-9]*) _c=0 ;; esac
  [ "$_c" -ge 2 ] || return 0
  kpp_pid >/dev/null || return 0          # nothing running: nothing to stop, the start is not an early exit
  _i=0
  while [ $_i -lt 72 ]; do
    _a="$(kpp_age)"; [ -n "$_a" ] && [ "$_a" -ge 305 ] && return 0
    [ $_i = 0 ] && kf_log "Amazon crash counter is $_c: waiting until KPPMainApp is 305 s old before restarting it"
    sleep 5; _i=$((_i + 1))
  done
  kf_log "WARNING: KPPMainApp did not reach 305 s; restarting anyway (counter $_c)"
}
restart_kpp() {
  crash_counter_guard
  stop kppmainapp >/dev/null 2>&1
  _i=0; while kpp_pid >/dev/null && [ $_i -lt 20 ]; do sleep 1; _i=$((_i + 1)); done
  start kppmainapp >/dev/null 2>&1
}
v32_guard_armed() { [ -f "$V32_GUARD_STATE" ] && [ "$(awk '{ print $1 }' "$V32_GUARD_STATE")" = armed ]; }
