#!/bin/bash
# 사용법: ./restore.sh
# backup/snapshot_lms_v1 의 파일을 원위치로 복구

BASE="/mnt/c/Users/baseg/OneDrive/바탕 화면/minici_ebpf"
B="$BASE/backup/snapshot_lms_v1"

if [ ! -d "$B" ]; then
    echo "[ERR] 백업 폴더가 없습니다: $B"
    exit 1
fi

cp "$B/include/protocol.h"  "$BASE/include/"
cp "$B/ipc_protocol.h"      "$BASE/"
cp "$B/server_ctx.h"        "$BASE/"
cp "$B/server/server_ctx.c" "$BASE/server/"
cp "$B/server/server.c"     "$BASE/server/"
cp "$B/client/client.c"     "$BASE/client/"
cp "$B/minici-tui.c"        "$BASE/"
cp "$B/Makefile"            "$BASE/"

echo "=== RESTORE DONE: snapshot_lms_v1 → 원위치 ==="
echo "이제 make clean && make 로 다시 빌드하세요."
