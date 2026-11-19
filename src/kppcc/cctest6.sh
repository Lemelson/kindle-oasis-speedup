#!/bin/sh
# Unit test of libkppcc.so (run inside the build container, see ../../build.sh test):
#   docker run --rm --network none --tmpfs /tmp:exec,size=200m --tmpfs /mnt:size=200m -v <repo>:/src:ro -v <out>:/out \
#       [-v <dir with your own cache files>:/real:ro] kppbuild sh /src/src/kppcc/cctest6.sh [lib]   (tmpfs: the 32-bit readdir fails on Docker overlay inode numbers)
# Prints PASS/FAIL lines; exit status = number of FAILs.
LIB=${1:-/out/libkppcc.so}; T=/out/test; C=/mnt/kfxcache/kpp; S=/tmp/store; F=0
ok() { echo "PASS $*"; }; bad() { echo "FAIL $*"; F=$((F + 1)); }
chk() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1: got '$2' want '$3'"; fi; }
mkdir -p /mnt/kfxcache; chmod 777 /mnt/kfxcache; rm -rf $C $S /tmp/cc.log; mkdir -p $C $S
cp $T/cctest6 /tmp/KPPMainAppcctest; cp $T/cctest6 /tmp/othercctest
E="KPPCC=1 KPPCC_STORE=$S KPPCC_LOG=/tmp/cc.log $CCX"   # CCX: extra env, e.g. the async switch (cctest6-async.sh)
N1=00000000000000000000000000000001; N2=00000000000000000000000000000002
echo "== 1. write through fopen/fclose/remove"
echo pre > $C/000000000000000000000000000000aa                     # invalid pre-existing file: not stored
env $E LD_PRELOAD=$LIB /tmp/KPPMainAppcctest write
chk "store has exactly the valid files" "$(ls $S | tr '\n' ' ')" "$N1 $N2 00000000000000000000000000000008 "
cmp -s $C/$N1 $S/$N1 && cmp -s $C/$N2 $S/$N2 && ok "store copies byte-identical (rewrite followed)" || bad "store copies differ"
( cd $C && md5sum $N1 $N2 ) > /tmp/a.md5; cp -p $S/$N1 /tmp/good1

echo "== 2. reboot: restore, corrupt store files dropped, good ones kept"
touch -d '2026-01-02 03:04:05' $S/$N1
rm -rf $C
head -c 100 $S/$N1 > $S/00000000000000000000000000000009                                  # truncated
{ head -c 48 $S/$N1; head -c $(( $(stat -c %s $S/$N1) - 48 )) /dev/zero; } > $S/0000000000000000000000000000000a   # right length, zero payload
: > $S/0000000000000000000000000000000b                                                  # empty
echo junk > $S/$N1.tmp; echo keep > $S/README                                             # leftover tmp removed, foreign file untouched
env $E LD_PRELOAD=$LIB /tmp/KPPMainAppcctest none
( cd $C && md5sum -c /tmp/a.md5 >/dev/null ) && ok "restored files identical" || bad "restored files differ"
chk "restored mtime kept" "$(stat -c %y $C/$N1 | cut -c1-19)" "2026-01-02 03:04:05"
chk "store after restore" "$(ls $S | tr '\n' ' ')" "$N1 $N2 00000000000000000000000000000008 README "
chk "cache after restore" "$(ls $C | tr '\n' ' ')" "$N1 $N2 00000000000000000000000000000008 "

echo "== 3. destination errors during restore must NOT delete the store"
for fault in write close rename open; do
  rm -rf $C; mkdir $C; before=$(ls $S | md5sum)
  env $E KPPFAULT=$fault LD_PRELOAD=$T/libfault.so:$LIB /tmp/KPPMainAppcctest none
  chk "restore with failing $fault: store unchanged" "$(ls $S | md5sum)" "$before"
  chk "restore with failing $fault: nothing half-written in the cache" "$(ls $C | wc -l)" "0"
  cmp -s $S/$N1 /tmp/good1 && ok "store file content intact ($fault)" || bad "store file changed ($fault)"
done
rm -rf $C; mkdir $C; mkdir $C/$N1.tmp                                 # tmp name blocked by a directory (EISDIR)
env $E LD_PRELOAD=$LIB /tmp/KPPMainAppcctest none
[ -f $S/$N1 ] && ok "blocked tmp name: store file kept" || bad "blocked tmp name: store file deleted"
rmdir $C/$N1.tmp

echo "== 4. destination errors while storing: previous copy survives, no tmp left"
rm -rf $C; mkdir $C; env $E LD_PRELOAD=$LIB /tmp/KPPMainAppcctest none           # clean restore
for fault in write close rename; do
  env $E KPPFAULT=$fault LD_PRELOAD=$T/libfault.so:$LIB /tmp/KPPMainAppcctest one $N1   # app rewrites N1 (same content), store copy fails
  cmp -s $S/$N1 /tmp/good1 && ok "store copy intact after failing $fault" || bad "store copy damaged after failing $fault"
  chk "no tmp left ($fault)" "$(ls $S | grep -c tmp)" "0"
done

echo "== 5. stale tmp files of a crashed restore are removed from the cache dir, other files are not touched"
echo x > $C/$N2.tmp; echo x > $C/foreign.tmp
env $E LD_PRELOAD=$LIB /tmp/KPPMainAppcctest none
chk "cache dir" "$(ls $C | tr '\n' ' ')" "$N1 $N2 00000000000000000000000000000008 foreign.tmp "
rm -f $C/foreign.tmp

echo "== 6. store size limit counts the new file"
rm -rf /tmp/store2; mkdir /tmp/store2; rm -rf $C; mkdir $C
env $CCX KPPCC=1 KPPCC_STORE=/tmp/store2 KPPCC_MAX_MB=1 LD_PRELOAD=$LIB /tmp/KPPMainAppcctest write >/dev/null 2>&1
for i in 0 1 2 3 4 5 6 7 8 9 a b c d e f; do env $CCX KPPCC=1 KPPCC_STORE=/tmp/store2 KPPCC_MAX_MB=1 LD_PRELOAD=$LIB /tmp/KPPMainAppcctest one 1000000000000000000000000000000$i; done
sz=$(du -sb /tmp/store2 | cut -f1); n=$(ls /tmp/store2 | wc -l)
[ $(cat /tmp/store2/* | wc -c) -le 1048576 ] && ok "store stays <= 1 MB ($(cat /tmp/store2/* | wc -c) bytes, $n files)" || bad "store above the limit"

echo "== 7. other program / switch off: nothing happens"
rm -rf $C; mkdir $C; n0=$(ls $S | wc -l)
env $E LD_PRELOAD=$LIB /tmp/othercctest write; chk "other program: cache files" "$(ls $C | wc -l)" "8"; chk "other program: store unchanged" "$(ls $S | wc -l)" "$n0"
rm -rf $C; mkdir $C
env KPPCC_STORE=$S LD_PRELOAD=$LIB /tmp/KPPMainAppcctest none; chk "switch off: nothing restored" "$(ls $C | wc -l)" "0"

echo "== 8. real cache files (Amazon's own output) are all accepted and restored byte-identically"
if [ -d /real ] && [ "$(ls /real | wc -l)" -gt 0 ]; then
  rm -rf /tmp/store3 $C; mkdir /tmp/store3 $C; cp /real/* /tmp/store3/; n=$(ls /real | wc -l)   # cache dir present: since 19.11 kpp5 recreates a missing one only from its own .dirmeta record (step 2 covers that)
  env $CCX KPPCC=1 KPPCC_STORE=/tmp/store3 KPPCC_LOG=/tmp/cc.log LD_PRELOAD=$LIB /tmp/KPPMainAppcctest none
  chk "real files restored" "$(ls $C | wc -l)" "$n"; chk "real files kept in the store" "$(ls /tmp/store3 | grep -c "^[0-9a-f]*$")" "$n"
  d=0; for f in /real/*; do cmp -s $f $C/${f##*/} || d=$((d + 1)); done; chk "real files differing after restore" "$d" "0"
  # every single-bit-flip / truncation of a real file: how many does the validator catch (informational)
else echo "SKIP no /real"; fi
echo "== log"; cat /tmp/cc.log
echo "== FAILS: $F"; exit $F
