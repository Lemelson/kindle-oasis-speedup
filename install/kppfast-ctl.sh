#!/bin/sh
# kppfast runtime toggle (no reinstall): edits /var/local/kppfast/{preload.list,env} and restarts KPPMainApp.
# usage: sh /var/local/kppfast/kppfast-ctl.sh <mode> [--no-restart]
#   stock     no LD_PRELOAD (empty preload.list)
#   fs        libkppfs.so only          x11   libkppx11.so only          all   every library in lib/
#   enable    clear the fail-safe (fails=0, disabled, start history) and restart with the current lists
#   restart   restart KPPMainApp (waits if Amazon's crash counter is >= 2, see kppfast-lib.sh)
#   nftd on|off   /var/local/DISABLE_NATIVE_THREAD_FREEZE_DETECTION (config switch read by libReactNative)
#   set KEY=VALUE / unset KEY   edit one line of env (KPP* keys only), e.g. set KPPFS_TTL_MS=500
#   status    = check.sh
D=/var/local/kppfast
KF_ROLE=ctl
. $D/kppfast-lib.sh || exit 1
R=1; [ "$2" = --no-restart ] || [ "$3" = --no-restart ] && R=0
lib() { [ -f $D/lib/$1 ] && echo $D/lib/$1; }
reset_failsafe() { echo 0 > /tmp/kppfast.fails; rm -f $D/disabled $D/starts; }
case "$1" in
  stock) : > $D/preload.list ;;
  fs)    lib libkppfs.so > $D/preload.list ;;
  x11)   lib libkppx11.so > $D/preload.list ;;
  all)   for so in $D/lib/*.so; do echo $so; done > $D/preload.list ;;
  enable) ;;
  restart) restart_kpp; kf_log "restarted"; exit 0 ;;
  nftd) case "$2" in on) touch /var/local/DISABLE_NATIVE_THREAD_FREEZE_DETECTION ;; off) rm -f /var/local/DISABLE_NATIVE_THREAD_FREEZE_DETECTION ;; *) echo "nftd on|off"; exit 2 ;; esac
        R=1; [ "$3" = --no-restart ] && R=0 ;;
  set) case "$2" in KPP[A-Z0-9_]*=*) k=${2%%=*}; grep -v "^$k=" $D/env > $D/env.tmp 2>/dev/null; echo "$2" >> $D/env.tmp; mv -f $D/env.tmp $D/env ;; *) echo "set KPPxxx=value"; exit 2 ;; esac ;;
  unset) grep -v "^$2=" $D/env > $D/env.tmp 2>/dev/null; mv -f $D/env.tmp $D/env ;;
  status) exec sh $D/check.sh ;;
  *) sed -n '2,12p' "$0"; exit 2 ;;
esac
reset_failsafe
kf_log "mode $*: preload.list [$(tr '\n' ' ' < $D/preload.list)] env [$(grep -v '^#' $D/env | tr '\n' ' ')] nftd=$( [ -e /var/local/DISABLE_NATIVE_THREAD_FREEZE_DETECTION ] && echo on || echo off)"
[ $R = 1 ] && { restart_kpp; kf_log "KPPMainApp restarted"; }
exit 0
