#!/bin/sh
# inside kindle-emu-rootfs: kpp4/db logtest (calls the firmware's kndk::log::log; syslog lands on stdout) under libkpplog
mkdir -p /var/tmp /var/local/kppfast; cp /d/logtest /var/tmp/KPPMainApplogtest; cp /d/logtest /var/tmp/otherlogtest
export LD_LIBRARY_PATH=/app/lib:/usr/lib; T=/var/tmp
$T/KPPMainApplogtest 2 > $T/stock.txt 2>&1
LD_PRELOAD=/t/libkpplog.so $T/KPPMainApplogtest 2 > $T/off.txt 2>&1
KPPLOG=1 LD_PRELOAD=/t/libkpplog.so $T/KPPMainApplogtest 2 > $T/quiet.txt 2>&1
KPPLOG=1 LD_PRELOAD=/t/libkpplog.so $T/otherlogtest 2 > $T/other.txt 2>&1
touch /var/local/kppfast/verbose-logs
KPPLOG=1 LD_PRELOAD=/t/libkpplog.so $T/KPPMainApplogtest 2 > $T/verbose.txt 2>&1
KPPLOG=1 LD_PRELOAD="/t/libkpplog.so /d/libkppdb.so" KPPDB_QUIETLOG=1 $T/KPPMainApplogtest 2 > $T/chain.txt 2>&1
wc -l $T/*.txt
for f in off other verbose; do echo "stock vs $f: $(diff $T/stock.txt $T/$f.txt | grep -c '^[<>]') differing lines (only the 'bound to' line may differ)"; done
echo "quiet keeps:"; sed 's/^syslog \([0-9]\): \[[0-9]*\] \[[^:]*:\([A-Z]*\)\].*/pri \1 \2/' $T/quiet.txt | sort | uniq -c
echo "stock has:"; sed 's/^syslog \([0-9]\): \[[0-9]*\] \[[^:]*:\([A-Z]*\)\].*/pri \1 \2/' $T/stock.txt | sort | uniq -c
echo "quiet lines identical to the stock lines of the kept levels: $(grep -E '^syslog [0-3]:|^syslog 6: .*DISABLED|^bound|^done' $T/stock.txt | sed 's/bound to library/bound to preload/' | diff - $T/quiet.txt | grep -c '^[<>]') diffs"
echo "chain (db QUIETLOG behind kpplog, verbose file present) vs stock: $(diff $T/stock.txt $T/chain.txt | grep -c '^<') lines removed"
