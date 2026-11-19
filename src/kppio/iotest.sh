#!/bin/sh
# kpp7 unit test of libkppio.so, inside kindle-emu-armcc:
#   docker run --rm --network none --tmpfs /tmp:exec,size=200m --tmpfs /mnt:size=200m --tmpfs /var/local:size=20m -v <repo>:/src:ro -v <out>:/out kppbuild sh /src/src/kppio/iotest.sh
# and on the firmware's own glibc 2.20 (driver iotest-k):  ... -e DRV=iotest-k <an image of your own firmware rootfs> /bin/sh /src/src/kppio/iotest.sh
# Prints PASS/FAIL lines; exit status = number of FAILs.
LIB=${1:-/out/libkppio.so}; T=/out/test; F=0; L=/tmp/io.log
ok() { echo "PASS $*"; }; bad() { echo "FAIL $*"; F=$((F + 1)); }
chk() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1: got '$2' want '$3'"; fi; }
D=/mnt/us/documents; mkdir -p $D /mnt/us/system/thumbnails /mnt/kfxcache/kpp /mnt/us/documents/b.sdr /var/local/kpp7t
B=$D/book.kfx; head -c 3000000 /dev/urandom > $B; printf 'line one\nline two\n' >> $B
head -c 400000 /dev/urandom | od -An -tx1 > $D/text.txt       # many newlines: fgets/getc paths
DRV=${DRV:-iotest}; cp $T/$DRV /tmp/KPPMainAppiotest; cp $T/$DRV /tmp/otheriotest
P=/tmp/KPPMainAppiotest; E="KPPIO=1 KPPIO_LOG=$L"

echo "== 1. identical results with every mode (digest of all returned bytes/values/errno)"
for f in $B $D/text.txt; do for seed in 1 2 3; do
  ref=$($P seq $f $seed 4000); ref64=$($P seq $f $seed 4000 x)
  for mode in "KPPIO_STAT=1" "KPPIO_STAT=1 KPPIO_TRACE=500" "KPPIO_BUF_KB=64" "KPPIO_BUF_KB=1024" "KPPIO_BUF_KB=8 KPPIO_STAT=1" "KPPIO_RA_KB=512 KPPIO_RA_TAIL_KB=256" "KPPIO_STAT=1 KPPIO_BUF_KB=256 KPPIO_RA_KB=4096 KPPIO_TRACE=100"; do
    rm -f $L
    got=$(env $E $mode LD_PRELOAD=$LIB $P seq $f $seed 4000); got64=$(env $E $mode LD_PRELOAD=$LIB $P seq $f $seed 4000 x)
    [ "$got" = "$ref" ] && [ "$got64" = "$ref64" ] && [ -n "$ref" ] || bad "digest differs: $(basename $f) seed $seed mode '$mode' ($got/$ref $got64/$ref64)"
  done; done; done
[ $F = 0 ] && ok "42 x 2 runs: digests equal to the run without the library"

echo "== 2. off / other program / master switch missing: no log, same digest"
ref=$($P seq $B 7 2000)
rm -f $L; got=$(env KPPIO_STAT=1 KPPIO_BUF_KB=64 KPPIO_LOG=$L LD_PRELOAD=$LIB $P seq $B 7 2000); chk "KPPIO unset: digest" "$got" "$ref"; [ ! -e $L ] && ok "KPPIO unset: no log" || bad "KPPIO unset wrote a log"
rm -f $L; got=$(env $E KPPIO_STAT=1 LD_PRELOAD=$LIB /tmp/otheriotest seq $B 7 2000); chk "other program: digest" "$got" "$ref"; [ ! -e $L ] && ok "other program: no log" || bad "other program wrote a log"
rm -f $L; got=$(env $E LD_PRELOAD=$LIB $P seq $B 7 2000); chk "KPPIO=1 without a mode: digest" "$got" "$ref"; [ ! -e $L ] && ok "no mode: no log" || bad "no mode wrote a log"
touch /mnt/us/DISABLE_KPPFAST; rm -f $L; env $E KPPIO_STAT=1 LD_PRELOAD=$LIB $P seq $B 7 200 >/dev/null; [ ! -e $L ] && ok "DISABLE_KPPFAST: no log" || bad "DISABLE_KPPFAST ignored"; rm -f /mnt/us/DISABLE_KPPFAST

echo "== 3. accounting"
rm -f $L; env $E KPPIO_STAT=1 KPPIO_TRACE=50 LD_PRELOAD=$LIB $P seq $B 5 3000 >/dev/null
grep -q "^kppio: pid .* stat=1 trace=50" $L && ok "header line" || bad "no header line"
grep -q "^B t=" $L && grep -q "^  book  *open=2 close=2 read=[0-9]*(+[0-9]* buffered) seek=[0-9]* write=0 sync=0 mmap=1" $L && ok "burst line with class book: $(grep '^  book' $L | head -1 | cut -c1-110)" || bad "no burst line for class book: $(cat $L | head -5)"
chk "trace lines capped" "$(grep -c '^T ' $L)" "50"
grep -q "^TOTAL book" $L && grep -q "^kppio: exit" $L && ok "totals at exit" || bad "no totals"
grep -q "thread [0-9]* KPPMainApp" $L && ok "thread line names the thread" || bad "no thread line"
rm -f $L; env $E KPPIO_STAT=1 LD_PRELOAD=$LIB $P write /mnt/kfxcache/kpp/x >/dev/null; grep -q "^TOTAL kfxcache .*write=1 sync=2" $L && ok "write/fsync/fdatasync counted in class kfxcache" || bad "kfxcache accounting: $(grep TOTAL $L)"
rm -f $L; env $E KPPIO_STAT=1 LD_PRELOAD=$LIB $P write /tmp/untracked >/dev/null; n=$(grep -c "^TOTAL" $L); grep -q "^TOTAL varlocal .*sync=2" $L && [ "$n" = 1 ] && ok "untracked path: only its fsyncs are listed" || bad "untracked path accounting: $(grep TOTAL $L)"
rm -f $L; env $E KPPIO_STAT=1 KPPIO_GAP_MS=100 LD_PRELOAD=$LIB $P idle $B 1500 & sleep 1; grep -q "^B t=" $L && ok "burst written by the helper thread while the process is still idle" || bad "idle burst missing: $(cat $L)"; wait
rm -f $L; out=$(env $E KPPIO_STAT=1 LD_PRELOAD=$LIB $P reuse $B); case "$out" in *ping) ok "descriptor number reused by a pipe works ($out)";; *) bad "reuse: $out";; esac
rm -f $L; env $E KPPIO_STAT=1 KPPIO_TRACE=20000 KPPIO_LOG_MAX_KB=8 LD_PRELOAD=$LIB $P seq $B 9 6000 >/dev/null; s=$(stat -c %s $L); [ $s -le 8300 ] && grep -q "log full" $L && ok "log capped ($s bytes)" || bad "log cap: $s bytes"

echo "== 4. threads, many open files, memory"
ref=$($P threads $B 3000 | tr '\n' ' ')
for mode in "KPPIO_STAT=1 KPPIO_TRACE=2000" "KPPIO_BUF_KB=64 KPPIO_RA_KB=1024 KPPIO_STAT=1"; do rm -f $L; got=$(env $E $mode LD_PRELOAD=$LIB $P threads $B 3000 | tr '\n' ' '); chk "6 threads, $mode" "$got" "$ref"; done
grep -q "^kppio: exit .* dropped 0" $L && ok "no accounting dropped: $(grep '^kppio: exit' $L)" || bad "exit line: $(grep '^kppio: exit' $L)"
ref=$($P many $B); got=$(env $E KPPIO_BUF_KB=64 LD_PRELOAD=$LIB $P many $B); chk "48 files open at once (more than the 32 buffer slots)" "$got" "$ref"
out=$(env $E KPPIO_BUF_KB=1024 KPPIO_RA_KB=512 LD_PRELOAD=$LIB $P leak $B 3000); g=${out#rss growth }; g=${g% kB}; [ "$g" -lt 1500 ] && ok "3000 open/close with a 1 MiB buffer: $out" || bad "leak: $out"
n=$(ls /proc/self/fd | wc -l); rm -f $L; env $E KPPIO_STAT=1 KPPIO_RA_KB=64 KPPIO_RA_TAIL_KB=64 LD_PRELOAD=$LIB $P seq $B 3 3000 >/dev/null; grep -q "ra queued [1-9][0-9]* done" $L && ok "read-ahead issued: $(grep -o 'ra queued.*' $L)" || bad "read-ahead: $(grep exit $L)"
echo "== $F failed"; exit $F
