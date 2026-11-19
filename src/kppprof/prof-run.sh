#!/bin/sh
# Mac side: profile Library page turns on the Kindle with libkppprof.so (stock app + profiler only).
#   sh src/kppprof/prof-run.sh [TURNS] [EXTRA_LIBS]
# EXTRA_LIBS: other kppfast libs to load as well (e.g. "libkppfs.so"); default none = stock behaviour.
# Leaves kppfast in the baseline config (device-baseline.sh) with KPPPROF unset; END_RESTART=1 restarts into it.
# EXTRA_LIBS example: "libkppfs.so libkppx11.so libkppmalloc.so libkppimg.so" (env keys must be set separately).
set -u
cd "$(dirname "$0")"
H=${KINDLE:-kindle}; TURNS=${1:-20}; EXTRA=${2:-}
K2=$(cd ../.. && pwd); BIN=${KPP_OUT:-$K2/out}; RUNS=${KPP_RUNS:-$K2/runs}
D=/var/local/kppfast
scp -q "$BIN/libkppprof.so" $H:/mnt/us/kpp2/ || exit 1
ssh $H "lipc-set-prop com.lab126.powerd preventScreenSaver 1
cp /mnt/us/kpp2/libkppprof.so $D/lib/ && chmod 644 $D/lib/libkppprof.so
grep -v ' libkppprof.so\$' $D/lib/SHA256SUMS > $D/lib/S.tmp; (cd $D/lib && sha256sum libkppprof.so) >> $D/lib/S.tmp; mv -f $D/lib/S.tmp $D/lib/SHA256SUMS
{ echo $D/lib/libkppprof.so; for l in $EXTRA; do echo $D/lib/\$l; done; } > $D/preload.list
sh $D/kppfast-ctl.sh set KPPPROF=1 --no-restart
rm -rf /tmp/kppprof; echo 0 > /tmp/kppfast.fails; rm -f $D/disabled; : > $D/starts
cat $D/preload.list"
LABEL=prof${EXTRA:+-$(echo $EXTRA | tr ' .' '__')}
KEEP_AWAKE=1 PRE_RESTART=/var/local/kpp2/prerestart.sh POST_RESTART=/var/local/kpp2/postrestart.sh MAXCRASH=99 \
  sh "$K2/bench/run-bench.sh" "$LABEL" 1 "$TURNS" > /tmp/prof-run-$$.txt 2>&1
tail -5 /tmp/prof-run-$$.txt
BD=$(sed -n 's/^results: //p' /tmp/prof-run-$$.txt)
PD="$RUNS/prof-$(basename "$BD" | sed 's/^kpp-bench-//')"; mkdir -p "$PD"
sleep 2
scp -q "$H:/tmp/kppprof/*" "$PD/"
# symbols for every executable mapping that got samples: pull the libs once into $RUNS/syms (your own device files: keep them private, never commit them)
SY="$RUNS/syms"; mkdir -p "$SY"
for p in $(awk '$2 ~ /x/ && $6 ~ /^\// {print $6}' "$PD"/*.maps | sort -u); do
  b=$(basename "$p"); [ -f "$SY/$b.sym" ] && continue
  scp -q "$H:$p" "$SY/$b" 2>/dev/null || continue
  docker run --rm --network none -v "$SY":/s ${KPP_IMAGE:-kppbuild} sh -c "nm -D --defined-only '/s/$b' > '/s/$b.sym' 2>/dev/null; nm --defined-only '/s/$b' >> '/s/$b.sym' 2>/dev/null; true"
done
ssh $H "sh $D/kppfast-ctl.sh unset KPPPROF --no-restart >/dev/null"; sh "$K2/bench/device-baseline.sh" ${END_RESTART:+--restart}
python3 analyze.py "$PD" "$BD" "$SY" | tee "$PD/report.txt"
