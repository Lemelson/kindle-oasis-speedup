#!/bin/sh
# Runs on the Kindle after a deliberate KPPMainApp restart. Closes Amazon dialogs that block input:
#  - "Application Error / could not be started": top border = black line >900 px on fb0 row 578, CLOSE at (988,996)
#  - "Collecting Debug Info" (after a crash on shutdown): bottom border = black line >900 px on row 1210,
#    CLOSE at (646,1112). Before looking, waits until Amazon's dump-stack has finished (it can take ~1 min).
# Never taps blind: on Home those spots are the Kindle+ promo / book covers.
rowrun() { dd if=/dev/fb0 bs=1280 skip=$1 count=1 2>/dev/null | od -An -v -tu1 | awk '{for(i=1;i<=NF;i++){if($i<80){c++; if(c>m)m=c}else c=0}} END{print m+0}'; }
w=0
while ps w | grep -q "[d]ump-stack" && [ $w -lt 180 ]; do [ $w = 0 ] && echo "dump-stack running (crash on shutdown), waiting"; sleep 3; w=$((w+3)); done
[ $w -gt 0 ] && sleep 5
i=0
while [ $i -lt 6 ]; do
  a=$(rowrun 578); b=$(rowrun 1210)
  if [ "$a" -gt 900 ]; then echo "error dialog on screen (row578 run $a): tapping CLOSE"; /var/local/kpp2/kinject tap /dev/input/event8 988 996 200
  elif [ "$b" -gt 900 ]; then echo "debug-info dialog on screen (row1210 run $b): tapping CLOSE"; /var/local/kpp2/kinject tap /dev/input/event8 646 1112 200
  else echo "no dialog (runs $a/$b)"; exit 0
  fi
  sleep 3; i=$((i+1))
done
