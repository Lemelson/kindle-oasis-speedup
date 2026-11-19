#!/bin/sh
# Runs inside the kppbuild container (see ../build.sh). /src = repo (read-only), /klibs = device glibc, /out = output.
set -e
K=/klibs; S=/src/src; O=/out; B=/tmp/b; mkdir -p $O $O/test $B
CF="-O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall -Wno-unused-parameter"
LNK="-Wl,--allow-shlib-undefined -Wl,--no-as-needed"
printf 'void *dlsym(void *h, const char *s){return 0;}\nvoid *dlopen(const char *f,int m){return 0;}\nint dladdr(const void *a, void *i){return 0;}\nchar *dlerror(void){return 0;}\n' > $B/dlstub.c
gcc -shared -fPIC -o $B/libdl.so.2 $B/dlstub.c -Wl,-soname,libdl.so.2
STUB=$B/libdl.so.2; DL=$K/libdl.so.2
so() { n=$1; shift; gcc -shared -fPIC -nostdlib -o $O/$n.tmp "$@" -lgcc $LNK -Wl,-soname,$n; mv -f $O/$n.tmp $O/$n
       echo "== $n $(stat -c %s $O/$n) bytes, glibc versions: $(readelf -W -V $O/$n | grep -o 'GLIBC_[0-9.]*' | sort -u | tr '\n' ' ')"; }

build_libs() {
so libkppfs.so $CF -D_FILE_OFFSET_BITS=32 -fvisibility=default -fno-delete-null-pointer-checks -Wno-nonnull-compare \
   $S/kppfs/kppfs.c $K/libc.so.6 $K/libpthread.so.0 $STUB
so libkppx11.so $CF -fno-builtin-printf -Wno-unused-function $S/kppx11/kppx11.c $K/libc.so.6 $K/libpthread.so.0 $STUB
so libkppmalloc.so $CF -mtp=cp15 -D_FILE_OFFSET_BITS=32 -fno-plt -ftls-model=initial-exec -fvisibility=default \
   -fno-builtin-malloc -fno-builtin-free -fno-builtin-calloc $S/kppmalloc/kppmalloc.c $K/libc.so.6 $K/libpthread.so.0 \
   -Wl,-z,now -Wl,--hash-style=both
so libkppimg.so $CF $S/kppimg/kppimg.c $K/libc.so.6 $STUB -Wl,-z,now
if [ -f /hot/hot_bytes.h ]; then
  so libkpphot.so $CF -marm -Wno-unused-result -I/hot $S/kpphot/kpphot.c $S/kpphot/kpphot_stub.S $K/libc.so.6 $STUB -Wl,-z,now
else echo "-- libkpphot.so skipped: no HOTBYTES (generate it from your own device with tools/genbytes.py)"; fi
so libkppside.so $CF -D_FILE_OFFSET_BITS=32 $S/kppside/kppside.c $K/libc.so.6 $K/libpthread.so.0 $DL -Wl,--hash-style=both
so libkppdb.so $CF -marm -D_FILE_OFFSET_BITS=32 -fvisibility=default -fno-delete-null-pointer-checks -Wno-misleading-indentation \
   -I$S/kppdb $S/kppdb/kppdb.c $K/libc.so.6 $K/libpthread.so.0 $STUB
for n in kpplog kppcc kppio; do
  so lib$n.so $CF -marm -D_FILE_OFFSET_BITS=32 -Wno-unused-result -I$S/$n $S/$n/$n.c $K/libc.so.6 $K/libpthread.so.0 $DL -Wl,--hash-style=both
done
so libkppgrip.so $CF -fvisibility=default $S/kppgrip/kppgrip.c $K/libc.so.6 $K/libpthread.so.0 $STUB -Wl,-z,now
so libkppprof.so $CF -D_FILE_OFFSET_BITS=32 $S/kppprof/kppprof.c $K/libc.so.6 $K/libpthread.so.0
gcc -O2 -static -march=armv7-a -mfpu=neon -mfloat-abi=hard -Wall -Wno-unused-result -o $O/kinject /src/bench/kinject.c && strip $O/kinject
echo "== kinject (static) $(stat -c %s $O/kinject) bytes"
(cd $O && sha256sum lib*.so kinject > SHA256SUMS)
}

build_tests() {  # test helpers use the container's glibc and never go to the device
T=$O/test
gcc -O1 -Wall -o $T/cctest6 $S/kppcc/cctest6.c
gcc -O1 -Wall -shared -fPIC -o $T/libfault.so $S/kppcc/fault.c -ldl
gcc -O1 -Wall -shared -fPIC -o $T/libX11.so.6 $S/kppgrip/fakex.c -Wl,-soname,libX11.so.6
gcc -O1 -Wall -shared -fPIC -o $T/libXi.so.6 $S/kppgrip/fakexi.c -Wl,-soname,libXi.so.6 -Wl,--no-as-needed $T/libX11.so.6
gcc -O1 -Wall -shared -fPIC -o $T/libfakegdk.so $S/kppgrip/fakegdk.c -Wl,-soname,libfakegdk.so
gcc -O1 -Wall -o $T/griptest $S/kppgrip/griptest.c $T/libfakegdk.so $T/libX11.so.6 -lpthread -ldl
gcc -O1 -Wall -Wno-unused-result -o $T/iotest $S/kppio/iotest.c -lpthread
echo "== running unit tests"; rc=0
sh $S/kppgrip/griptest.sh > $T/griptest.log 2>&1 || rc=$((rc + 1)); tail -1 $T/griptest.log
sh $S/kppcc/cctest6.sh > $T/cctest6.log 2>&1 || rc=$((rc + 1)); tail -1 $T/cctest6.log
sh $S/kppio/iotest.sh > $T/iotest.log 2>&1 || rc=$((rc + 1)); tail -1 $T/iotest.log
return $rc
}

case "${1:-libs}" in
  libs) build_libs ;;
  test) build_libs; build_tests ;;
  *) echo "usage: build-in-container.sh libs|test" >&2; exit 2 ;;
esac
