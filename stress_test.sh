#!/bin/bash
echo -e "\033[1;36m======================================================\033[0m"
echo -e "\033[1;33m       🚀 미니 CI 서버 멀티스레드 동시 부하 테스트 🚀       \033[0m"
echo -e "\033[1;36m======================================================\033[0m"
echo "서버의 4개 스레드를 동시에 100% 가동시키기 위해,"
echo "2초 동안 죽지 않는 무한 루프 폭탄 4개를 동시에 투하합니다!"
echo ""

# 에디터가 열리는 것을 방지하기 위해 'n'을 입력 파이프로 넘겨주며 백그라운드(&) 실행
echo "n" | ./client_bin test_03_timeout.c > /dev/null &
echo "n" | ./client_bin test_03_timeout.c > /dev/null &
echo "n" | ./client_bin test_03_timeout.c > /dev/null &
echo "n" | ./client_bin test_03_timeout.c > /dev/null &

echo "💣 폭탄 투하 완료! 서버 터미널(UI)을 확인하세요!"
echo "(스레드 0, 1, 2, 3이 동시에 RUNNING 상태로 변하는 것을 볼 수 있습니다.)"

# 백그라운드 작업이 모두 끝날 때까지 대기
wait
echo -e "\n\033[1;32m✅ 4개의 스레드가 모든 처리를 완료했습니다!\033[0m"
