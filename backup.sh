#!/bin/bash
BASE="/mnt/c/Users/baseg/OneDrive/바탕 화면/minici_ebpf"
B="$BASE/backup/snapshot_lms_v3"

mkdir -p "$B/server" "$B/client" "$B/include"
cp "$BASE/include/protocol.h"   "$B/include/"
cp "$BASE/ipc_protocol.h"       "$B/"
cp "$BASE/server_ctx.h"         "$B/"
cp "$BASE/server/server_ctx.c"  "$B/server/"
cp "$BASE/server/server.c"      "$B/server/"
cp "$BASE/client/client.c"      "$B/client/"
cp "$BASE/minici-tui.c"         "$B/"
cp "$BASE/Makefile"             "$B/"

echo "=== BACKUP DONE: snapshot_lms_v3 ==="
echo ""
ls -la "$B"
