#!/bin/sh
# Mac side: put the Kindle back to the accepted kppfast configuration (no restart unless --restart).
# Baseline 19.11.2026 01:25 CET:
#   fs, x11, malloc, img                      first accelerators (kpp2/kpp3)
#   hot-v4   5 exact hot patches (as v3) + KPPHOT_RSEL_EDGE=700: selection page turn at the edge after 0.7 s (reader)
#   side     weblab records not sent, isColorEnabled asked once
#   db       prepared-statement reuse
#   log      ONE switch for KPPMainApp log spam: `touch /var/local/kppfast/verbose-logs` = every line as stock,
#            `rm` = quiet again (no restart needed)
#   cc       Amazon's cover cache: 16 MB instead of 2 MB (KPPCC_CAP_MB) and kept across reboots (KPPCC, copies in
#            /var/local/kppfast/covercache) - approved by the device owner 19.11.2026 (cached tiles are Amazon's own 4-bit variant)
#            + KPPCC_BOOK_MB=16 / KPPCC_BOOK=1 (19.11 03:45): Amazon's whole-item tile cache /mnt/kfxcache/kppbook
#            16 MB instead of 1 MB and mirrored to covercache/book - approved by the device owner 19.11.2026
#   grip     text selection: finger lift at the screen edge is no longer lost (reader)
# Previous baselines: device-baseline-20261119b.sh (without the item cache), device-baseline-20261119a.sh (without cc/grip/v4), device-baseline-20261118.sh (fs,x11,malloc,img).
H=${KINDLE:-kindle}; D=/var/local/kppfast
ssh $H "printf '$D/lib/libkppfs.so\n$D/lib/libkppx11.so\n$D/lib/libkppmalloc.so\n$D/lib/libkppimg.so\n$D/lib/libkpphot.so\n$D/lib/libkppside.so\n$D/lib/libkppdb.so\n$D/lib/libkpplog.so\n$D/lib/libkppcc.so\n$D/lib/libkppgrip.so\n' > $D/preload.list.new
printf 'KPPX11_SHMPUT=1\nKPPX11_SHMGET=1\nKPPFS_STAT=1\nKPPFS_TTL_MS=1000\nKPPFS_NEG=1\nKPPFS_CLOCK=0\nKPPX11_PIXBUFWIN=1\nKPPMALLOC=1\nKPPIMG=1\nKPPHOT_GDKGRAY=1\nKPPHOT_CAIROGET=1\nKPPHOT_PIXDIV=1\nKPPHOT_PIXLDIV=1\nKPPHOT_SINMEMO=1\nKPPHOT_RSEL_EDGE=700\nKPPSIDE_WEBLAB=1\nKPPSIDE_DEVCAP=1\nKPPDB_STMTCACHE=1\nKPPLOG=1\nKPPCC=1\nKPPCC_CAP_MB=16\nKPPCC_BOOK_MB=16\nKPPCC_BOOK=1\nKPPGRIP=1\n' > $D/env.new
mv -f $D/env.new $D/env; mv -f $D/preload.list.new $D/preload.list   # env first: a restart in between must never see new libs without their switches... (every switch defaults to off)
cat $D/preload.list; tr '\n' ' ' < $D/env; echo
[ '$1' = --restart ] && { sh /var/local/kpp2/prerestart.sh >/dev/null 2>&1; sh $D/kppfast-ctl.sh restart; sleep 20; sh /var/local/kpp2/postrestart.sh; }
sh /var/local/kpp2/keepawake.sh >/dev/null"
