#!/bin/sh
# Unit test of libkppgrip.so with fake X/GDK (run inside the build container, see ../../build.sh test).
LIB=${1:-/out/libkppgrip.so}; T=/out/test; F=0
cp $T/griptest /tmp/KPPMainAppgriptest; cp $T/griptest /tmp/othergriptest
export LD_LIBRARY_PATH=$T KPPGRIP_STATE=/tmp/kppgrip.state KPPFAKEX_PROP=/tmp/fakex.prop KPPGRIP_LOG=/tmp/grip.log
run() { printf '\001' > $KPPFAKEX_PROP; }      # property = 1 like on the device
st() { if [ "$2" = "$3" ]; then echo "PASS $1"; else echo "FAIL $1 (got $2 want $3)"; F=$((F + 1)); fi; }
rm -f $KPPGRIP_STATE /tmp/grip.log
echo "== main"; run; KPPGRIP=1 KPPGRIP_MAX_MS=1000 KPPGRIP_FOCUS_MS=200 LD_PRELOAD=$LIB /tmp/KPPMainAppgriptest main; F=$((F + $?))
echo "== property events (KPPGRIP_XEV=1)"; run; KPPGRIP=1 KPPGRIP_XEV=1 LD_PRELOAD=$LIB /tmp/KPPMainAppgriptest xev; F=$((F + $?))
echo "== gate off"; run; KPPGRIP=1 KPPGRIP_GATE=0 LD_PRELOAD=$LIB /tmp/KPPMainAppgriptest nogate; F=$((F + $?))
echo "== switch off"; run; LD_PRELOAD=$LIB /tmp/KPPMainAppgriptest off; F=$((F + $?))
echo "== other program"; run; KPPGRIP=1 LD_PRELOAD=$LIB /tmp/othergriptest other; F=$((F + $?))
echo "== normal exit with the finger down"; run; KPPGRIP=1 LD_PRELOAD=$LIB /tmp/KPPMainAppgriptest exit; st "process saw grip off before exit" $? 0
st "destructor restored the property" "$(od -An -tu1 $KPPFAKEX_PROP | tr -d ' ')" 1; st "state file gone" "$(ls $KPPGRIP_STATE 2>/dev/null)" ""
echo "== crash with the finger down, then restart"; run; KPPGRIP=1 LD_PRELOAD=$LIB /tmp/KPPMainAppgriptest crash 2>/dev/null
st "after the crash the property is still 0" "$(od -An -tu1 $KPPFAKEX_PROP | tr -d ' ')" 0; st "state file left behind" "$(cut -d' ' -f2 $KPPGRIP_STATE)" 1
KPPGRIP=1 LD_PRELOAD=$LIB /tmp/KPPMainAppgriptest recover; F=$((F + $?))
run; KPPGRIP=1 LD_PRELOAD=$LIB /tmp/KPPMainAppgriptest crash 2>/dev/null; printf '\002' > $KPPFAKEX_PROP
KPPGRIP=1 LD_PRELOAD=$LIB /tmp/KPPMainAppgriptest recover-ext; F=$((F + $?))
echo "== log"; cat /tmp/grip.log
echo "== FAILS: $F"; exit $F
