# snapshot_lms_v1 — 백업 스냅샷

저장 시각: 2026-05-24

## 포함 파일

| 파일 | 경로 |
|------|------|
| protocol.h | include/protocol.h |
| ipc_protocol.h | ipc_protocol.h |
| server_ctx.h | server_ctx.h |
| server_ctx.c | server/server_ctx.c |
| server.c | server/server.c  ← WSL에서 복사 필요 |
| client.c | client/client.c |
| minici-tui.c | minici-tui.c |
| Makefile | Makefile |

## 복구 방법

WSL 터미널에서:

```bash
cd /mnt/c/Users/baseg/OneDrive/바탕\ 화면/minici_ebpf
B=backup/snapshot_lms_v1
cp $B/include/protocol.h  include/
cp $B/ipc_protocol.h      .
cp $B/server_ctx.h        .
cp $B/server/server_ctx.c server/
cp $B/server/server.c     server/
cp $B/client/client.c     client/
cp $B/minici-tui.c        .
cp $B/Makefile            .
make clean && make
```

## server.c 백업 주의

server.c 는 1050줄로 커서 WSL에서 직접 복사했을 때만 완전합니다.
나머지 파일들은 이 폴더에 완전하게 저장되어 있습니다.
