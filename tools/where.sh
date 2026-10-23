#!/bin/sh
# Run the game for a few seconds, then show where it is (backtrace + CPU state).
cd "$(dirname "$0")/.."
ROM=${ROM:-/mnt/user-data/uploads/dqix-decomp/extract/baserom_dqix_usa.nds}
EXT=${EXT:-/mnt/user-data/uploads/dqix-decomp/extract/usa}
./dqix "$ROM" "$EXT" ${FRAMES:-0} ${LOG:-2} ${TMPDIR:-/tmp}/dqix.sav > ${TMPDIR:-/tmp}/run.log 2>&1 &
sleep ${SECS:-6}
P=$(pgrep -f "^./dqix" | head -1)
[ -z "$P" ] && { echo "exited:"; tail -40 ${TMPDIR:-/tmp}/run.log; exit; }
gdb -q -batch -p $P -ex "bt ${DEPTH:-14}" -ex "p/x g_arm9.r" -ex "p g_frame" -ex "p g_vcount" \
    -ex "p/x g_c9.ime" -ex "p/x g_c9.ie" -ex "p/x g_c9.iflags" -ex "p/x g_c7.ie" -ex "p/x g_c7.iflags" -ex "p/x g_arm7.r" -ex "p/x g_arm9.cpsr_ctl" 2>&1 \
    | grep -v "^\[New\|^Reading\|warning\|libthread\|Thread debugging\|detached" | sed 's/ (c=c@entry=0x[0-9a-f]* <g_arm9>)//; s/ (c=0x[0-9a-f]* <g_arm9>)//'
kill $P
echo "--- log tail"; tail -15 ${TMPDIR:-/tmp}/run.log
