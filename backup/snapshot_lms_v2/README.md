# snapshot_lms_v2 — 백업 스냅샷

저장 시각: 2026-05-24 18:41

## v1 → v2 변경 내용
- **minici-tui.c 전면 개편**
  - 메뉴 항목 전부 한국어 + 설명 문구 표시
  - 방향키 선택 + Enter 실행 방식
  - 제출 기한 설정 기능 (헤더 상단 표시)
  - test.sh 경로 설정 + 학생 배포 기능

## 이 폴더에 저장된 파일 (변경분만)
- minici-tui.c  ✅ (v2 핵심 변경)

## 나머지 파일 위치
변경 없는 파일은 snapshot_lms_v1 에서 가져오면 됩니다:
- include/protocol.h
- ipc_protocol.h
- server_ctx.h
- server/server_ctx.c
- server/server.c
- client/client.c
- Makefile

## 복구 방법 (WSL)
```bash
cd /mnt/c/Users/baseg/OneDrive/바탕\ 화면/minici_ebpf
# 나머지는 v1에서
B1=backup/snapshot_lms_v1
cp $B1/include/protocol.h  include/
cp $B1/ipc_protocol.h      .
cp $B1/server_ctx.h        .
cp $B1/server/server_ctx.c server/
cp $B1/server/server.c     server/
cp $B1/client/client.c     client/
cp $B1/Makefile            .
# TUI는 v2에서
cp backup/snapshot_lms_v2/minici-tui.c .
make clean && make
```
