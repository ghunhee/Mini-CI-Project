#!/bin/bash
echo "Building client..."
make client_bin

echo "============================================="
echo "   🛡️ Mini CI Server Security Stress Test    "
echo "============================================="

echo "🚀 1. 정상 제출 (AC)"
./client_bin test_ac.c &

echo "🚀 2. 오답 제출 (WA)"
./client_bin test_wa.c &

echo "🚀 3. CPU 무한루프 (TLE - 2초 후 강제종료)"
./client_bin test_tle.c &

echo "🚨 4. 서버 파일 탈취 해킹 (즉결 사살)"
./client_bin test_snoop.c &

echo "🚨 5. 외부 네트워크 접속 해킹 (즉결 사살)"
./client_bin test_network.c &

wait
echo "============================================="
echo "   ✅ 모든 테스트 케이스 제출 완료! "
echo "   서버 터미널의 Ncurses UI 대시보드를 확인하세요! "
echo "============================================="
