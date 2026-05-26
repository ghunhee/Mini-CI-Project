#!/bin/bash
# 최종 서버 제출용 스크립트 (GitHub 푸시 및 원격 자동 채점 시뮬레이션)
# --watch 플래그: 파일 저장 시 자동 재채점 모드

# ── --watch 플래그 파싱 ──────────────────────────────────────────
WATCH_MODE=0
ARGS=()

for arg in "$@"; do
    if [ "$arg" = "--watch" ] || [ "$arg" = "-w" ]; then
        WATCH_MODE=1
    else
        ARGS+=("$arg")
    fi
done

# 파싱 후 남은 인자가 없으면 사용법 출력
if [ ${#ARGS[@]} -eq 0 ]; then
    echo "사용법: ./submit.sh [--watch] <소스파일1.c> [소스파일2.c ...]"
    echo ""
    echo "  --watch, -w    파일 저장 시 자동 재채점 모드 활성화"
    exit 1
fi

# ── 공통 바이너리 존재 확인 ──────────────────────────────────────
if [ ! -f "./client_bin" ]; then
    echo "[❌ Error] client_bin 파일이 없습니다."
    exit 1
fi

# ── Watch Mode ────────────────────────────────────────────────────
if [ "$WATCH_MODE" -eq 1 ]; then
    echo "=============================================="
    echo "👁  Watch Mode: 파일 감시 채점을 시작합니다..."
    echo "=============================================="

    # inotify_watch 바이너리 존재 확인 및 자동 빌드
    if [ ! -f "./inotify_watch" ]; then
        echo "[*] inotify_watch 빌드 중..."
        gcc -O2 -o inotify_watch inotify_watch.c
        if [ $? -ne 0 ]; then
            echo "[❌ Error] inotify_watch 빌드 실패"
            exit 1
        fi
        echo "[✓] 빌드 완료"
    fi

    # inotify_watch 실행: client_bin 경로와 감시할 파일 목록 전달
    ./inotify_watch ./client_bin "${ARGS[@]}"
    exit $?
fi

# ── 일반 제출 모드 (기존 동작 그대로) ───────────────────────────
echo "=============================================="
echo "🚀 GitHub 채점 서버(Mini CI)로 코드를 제출합니다..."
echo "=============================================="

./client_bin "${ARGS[@]}"
