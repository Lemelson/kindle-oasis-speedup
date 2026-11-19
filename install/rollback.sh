#!/bin/sh
# kppfast rollback: put the stock /etc/upstart/kppmainapp.conf back and remove /var/local/kppfast.
# usage: sh rollback.sh [--keep-files] [--restart]
#   --keep-files   leave /var/local/kppfast in place (only the job line is removed)
#   --restart      restart KPPMainApp afterwards (otherwise the stock launch starts with the next start / reboot)
# Without SSH: create /mnt/us/DISABLE_KPPFAST over USB -> every following start is stock (the job line and
# launch.sh both honour it); this script then removes the rest at leisure.
HERE="$(cd "$(dirname "$0")" && pwd)"
KF_ROLE=rollback
. "$HERE/kppfast-lib.sh" 2>/dev/null || . /var/local/kppfast/kppfast-lib.sh || { echo "kppfast-lib.sh not found"; exit 1; }
KEEP=0; RESTART=0
for a in "$@"; do case "$a" in --keep-files) KEEP=1 ;; --restart) RESTART=1 ;; esac; done
mkdir -p "$KF_BACKUPS" 2>/dev/null
kf_log "=== rollback"
[ "$(id -u)" = 0 ] || kf_fail "must run as root"
ST="$(conf_state)"; kf_log "kppmainapp.conf: $ST"
if [ "$ST" = kppfast ]; then
  SRC=""
  for c in "$KF_STOCK_COPY" $(ls -t "$KF_BACKUPS"/kppmainapp.conf.before-kppfast.* 2>/dev/null); do
    [ "$(sha256_of "$c")" = "$CONF_STOCK_SHA256" ] && { SRC="$c"; break; }
  done
  if [ -z "$SRC" ]; then   # rebuild from the current file: drop exactly our line
    grep -v -x -F "$KF_LINE" "$KF_CONF" > /tmp/kppmainapp.conf.rb.$$
    [ "$(sha256_of /tmp/kppmainapp.conf.rb.$$)" = "$CONF_STOCK_SHA256" ] && SRC=/tmp/kppmainapp.conf.rb.$$
  fi
  [ -n "$SRC" ] || kf_fail "no verified stock copy of the job file found"
  WAS="$(root_mount_mode)"
  mntroot rw >/dev/null 2>&1
  [ "$(root_mount_mode)" = rw ] || kf_fail "root not writable after mntroot rw"
  cp "$SRC" "$KF_CONF.kf.$$" && chmod 0644 "$KF_CONF.kf.$$" && mv -f "$KF_CONF.kf.$$" "$KF_CONF"; sync
  [ "$WAS" = rw ] || mntroot ro >/dev/null 2>&1
  rm -f /tmp/kppmainapp.conf.rb.$$
  [ "$(conf_state)" = stock ] || kf_fail "job file is still not stock ($(sha256_of $KF_CONF))"
  kf_log "stock kppmainapp.conf restored from $SRC; root mount now: $(root_mount_mode)"
elif [ "$ST" = stock ]; then
  kf_log "job file already stock"
else
  kf_log "WARNING: job file unknown ($(sha256_of $KF_CONF)); not touching it"
fi
if [ $KEEP = 0 ] && [ "$(conf_state)" = stock ]; then
  cp "$KF_DIR/launch.log" "$KF_BACKUPS/kppfast-launch.log.$TS" 2>/dev/null
  rm -rf "$KF_DIR" && kf_log "removed $KF_DIR (launch.log saved to $KF_BACKUPS)"
fi
[ $RESTART = 1 ] && { restart_kpp; kf_log "KPPMainApp restarted"; }
kf_log "rollback done"
exit 0
