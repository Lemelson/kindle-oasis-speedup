#!/bin/sh
# Mac side: push kinject + kpp-bench.sh to the Kindle, run the benchmark, fetch results, summarise.
#   sh bench/run-bench.sh LABEL [REPS] [TURNS]      (env passed through: KEY_FWD KEY_BACK LIB_X LIB_Y IRQ QUIET RESTART)
set -eu
LABEL=${1:?label}; REPS=${2:-3}; TURNS=${3:-20}
H=${KINDLE:-kindle}; K2=$(cd "$(dirname "$0")/.." && pwd); BIN=${KPP_OUT:-$K2/out}; RUNS=${KPP_RUNS:-$K2/runs}
ssh $H 'mkdir -p /mnt/us/kpp2 /var/local/kpp2'
scp -q "$BIN/kinject" "$K2/bench/kpp-bench.sh" "$K2/bench/dismiss-error.sh" "$K2/bench/wake.sh" $H:/mnt/us/kpp2/
ssh $H 'cp /mnt/us/kpp2/kinject /mnt/us/kpp2/kpp-bench.sh /mnt/us/kpp2/dismiss-error.sh /mnt/us/kpp2/wake.sh /var/local/kpp2/ && chmod 755 /var/local/kpp2/kinject'
ENV=""; for v in KEY_FWD KEY_BACK LIB_X LIB_Y IRQ QUIET PER RESTART GAP_US JOB MAXCRASH PRE_RESTART POST_RESTART KEEP_AWAKE; do eval "x=\${$v:-}"; [ -n "$x" ] && ENV="$ENV $v=$x"; done
REMOTE=$(ssh $H "env $ENV sh /var/local/kpp2/kpp-bench.sh $LABEL $REPS $TURNS" | tail -1)
D="$RUNS/kpp-bench-$(basename "$REMOTE")"; mkdir -p "$D"
scp -q "$H:$REMOTE/*" "$D/"
echo "results: $D"
python3 "$K2/bench/summarize.py" "$D"
