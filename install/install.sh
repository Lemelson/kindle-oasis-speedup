#!/bin/sh
# kppfast installer (Kindle Oasis 3, FW 5.18.2, busybox sh, run as root).
# usage: sh install.sh [payload dir]     (default: the directory of this script)
# The payload dir holds: launch.sh kppfast-lib.sh kppfast-ctl.sh rollback.sh check.sh lib/*.so lib/SHA256SUMS SHA256SUMS
#
# What it changes:
#   /var/local/kppfast/            (ext3, rw, exec allowed) launch.sh, kppfast-*.sh, lib/*.so + lib/SHA256SUMS,
#                                  env (missing keys added from env.default, user values kept), preload.list (= all),
#                                  kppmainapp.conf.stock (verified copy of the stock job for rollback)
#   /etc/upstart/kppmainapp.conf   ONE line added before "exec /app/bin/KPPMainApp" (see kppfast-lib.sh KF_LINE);
#                                  the only rootfs write (~200 bytes; mntroot rw -> staged copy + rename -> mntroot ro)
# Not touched: /app/bin/KPPMainAppV2, /opt/amazon/ebook/lib, cc.db.
# Backups: /mnt/us/kppfast-logs/backups/kppmainapp.conf.before-kppfast.<TS>. Log: .../kppfast.log
# The new launch takes effect at the next KPPMainApp start (reboot, or kppfast-ctl.sh restart).
P="$(cd "${1:-$(dirname "$0")}" && pwd)" || exit 1
KF_ROLE=install
. "$P/kppfast-lib.sh" || { echo "cannot read $P/kppfast-lib.sh"; exit 1; }
mkdir -p "$NRR" "$KF_BACKUPS" 2>/dev/null

kf_log "=== install from $P"
[ "$(id -u)" = 0 ] || kf_fail "must run as root"
firmware_ok || kf_fail "firmware is not $EXPECTED_FIRMWARE_BUILD"
# only on the author's device: a separate reader patch has a boot guard; wait until it has finished
v32_guard_armed && kf_fail "the v32 boot guard is still armed ($(cat $V32_GUARD_STATE)); install after it says 'ok'"
# 1. payload integrity (SHA-256 pinned in SHA256SUMS)
[ -f "$P/SHA256SUMS" ] || kf_fail "missing $P/SHA256SUMS"
while read -r SUM F; do
  [ -n "$SUM" ] || continue
  [ "$(sha256_of "$P/$F")" = "$SUM" ] || kf_fail "payload file damaged: $F"
done < "$P/SHA256SUMS"
for f in launch.sh kppfast-lib.sh kppfast-ctl.sh rollback.sh check.sh lib/SHA256SUMS; do
  grep -q " $f\$" "$P/SHA256SUMS" || kf_fail "$f not covered by SHA256SUMS"
done
ls "$P"/lib/*.so >/dev/null 2>&1 || kf_fail "no libraries in $P/lib"
# 2. job file state
ST="$(conf_state)"
kf_log "kppmainapp.conf: $ST ($(sha256_of $KF_CONF))"
[ "$ST" = unknown ] && kf_fail "$KF_CONF is neither the stock 5.18.2 job nor the kppfast one; not touching it"
_need="$(du -sk "$P/lib" | awk '{ print $1 + 64 }')"; _have="$(free_kb /var/local)"
[ -n "$_have" ] && [ "$_have" -gt "$_need" ] || kf_fail "not enough space on /var/local (${_have:-?} KB free, need $_need KB)"

# 3. /var/local/kppfast (no rootfs involved)
mkdir -p "$KF_DIR/lib.new" || kf_fail "cannot create $KF_DIR"
rm -f "$KF_DIR"/lib.new/*
cp "$P"/lib/*.so "$P/lib/SHA256SUMS" "$KF_DIR/lib.new/" || kf_fail "copy of libraries failed"
( cd "$KF_DIR/lib.new" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) || kf_fail "copied libraries do not verify"
chmod 0644 "$KF_DIR"/lib.new/*
rm -rf "$KF_DIR/lib.old"; [ -d "$KF_DIR/lib" ] && mv "$KF_DIR/lib" "$KF_DIR/lib.old"
mv "$KF_DIR/lib.new" "$KF_DIR/lib" && rm -rf "$KF_DIR/lib.old"
for f in launch.sh kppfast-lib.sh kppfast-ctl.sh rollback.sh check.sh; do
  cp "$P/$f" "$KF_DIR/$f.new" && chmod 0755 "$KF_DIR/$f.new" && mv -f "$KF_DIR/$f.new" "$KF_DIR/$f" || kf_fail "copy of $f failed"
  [ "$(sha256_of "$KF_DIR/$f")" = "$(sha256_of "$P/$f")" ] || kf_fail "$KF_DIR/$f does not verify"
done
# env: keep the user's values, add keys from env.default that are missing
touch "$KF_DIR/env"
if [ -f "$P/env.default" ]; then
  grep -E '^KPP[A-Z0-9_]*=' "$P/env.default" | while IFS= read -r L; do
    grep -q "^${L%%=*}=" "$KF_DIR/env" || echo "$L" >> "$KF_DIR/env"
  done
fi
# default mode after every install: all (every lib/*.so); A/B later with kppfast-ctl.sh stock|fs|x11|all
for so in "$KF_DIR"/lib/*.so; do echo "$so"; done > "$KF_DIR/preload.list"
echo 0 > /tmp/kppfast.fails; rm -f "$KF_DIR/fails"; rm -f "$KF_DIR/disabled" "$KF_DIR/starts"
kf_log "staged $KF_DIR: libs [$(cd $KF_DIR/lib && ls *.so | tr '\n' ' ')] preload.list [$(tr '\n' ' ' < $KF_DIR/preload.list)] env [$(grep -v '^#' $KF_DIR/env | tr '\n' ' ')]"

# 4. the one-line rootfs change
if [ "$ST" = kppfast ]; then
  kf_log "job file already has the kppfast line; rootfs untouched"
else
  cp "$KF_CONF" "$KF_BACKUPS/kppmainapp.conf.before-kppfast.$TS" &&
    [ "$(sha256_of "$KF_BACKUPS/kppmainapp.conf.before-kppfast.$TS")" = "$CONF_STOCK_SHA256" ] || kf_fail "backup of $KF_CONF failed"
  cp "$KF_CONF" "$KF_STOCK_COPY" && [ "$(sha256_of "$KF_STOCK_COPY")" = "$CONF_STOCK_SHA256" ] || kf_fail "copy of the stock job to $KF_STOCK_COPY failed"
  make_kf_conf "$KF_STOCK_COPY" "$KF_DIR/kppmainapp.conf.kppfast" || kf_fail "could not build the kppfast job file"
  [ "$(sha256_of "$KF_DIR/kppmainapp.conf.kppfast")" = "$CONF_KF_SHA256" ] || kf_fail "built job file has an unexpected hash"
  [ "$(free_kb /)" -gt 16 ] 2>/dev/null || kf_fail "rootfs is full"
  WAS="$(root_mount_mode)"
  mntroot rw >/dev/null 2>&1
  [ "$(root_mount_mode)" = rw ] || kf_fail "root not writable after mntroot rw ($(root_mount_mode))"
  ok=0
  cp "$KF_DIR/kppmainapp.conf.kppfast" "$KF_CONF.kf.$$" && chmod 0644 "$KF_CONF.kf.$$" &&
    mv -f "$KF_CONF.kf.$$" "$KF_CONF" && sync && [ "$(sha256_of "$KF_CONF")" = "$CONF_KF_SHA256" ] && ok=1
  if [ $ok != 1 ]; then
    kf_log "ERROR: writing $KF_CONF failed; restoring the stock job"
    rm -f "$KF_CONF.kf.$$"; cp "$KF_STOCK_COPY" "$KF_CONF"; sync
  fi
  [ "$WAS" = rw ] || { mntroot ro >/dev/null 2>&1; }
  kf_log "root mount now: $(root_mount_mode)$( [ "$(root_mount_mode)" = rw ] && [ "$WAS" != rw ] && echo ' (mntroot ro failed: harmless, a reboot remounts ro)')"
  [ $ok = 1 ] || kf_fail "job file not changed"
  kf_log "kppmainapp.conf: kppfast line added (sha256 $CONF_KF_SHA256)"
fi
kf_log "installed. Takes effect at the next KPPMainApp start (reboot, or: sh $KF_DIR/kppfast-ctl.sh restart)"
exit 0
