#!/bin/bash
# 로컬 테스트용 스크립트 (학생이 제출 전 자기 컴퓨터에서 검증하는 용도)

if [ -z "$1" ]; then
    echo "사용법: ./test.sh <소스파일.c>"
    exit 1
fi

for SRC in "$@"; do
    echo "------------------------------------------------"
    BASE="${SRC%.c}"

    if [ ! -f "$SRC" ]; then
        echo "[❌ Error] $SRC 파일을 찾을 수 없습니다. 건너뜁니다."
        continue
    fi

    echo "[로컬 테스트] 1. $SRC 컴파일 중..."
    gcc -O2 -Wall -o local_test_run "$SRC"
    if [ $? -ne 0 ]; then
        echo -e "\033[31m[❌ Error] 컴파일 에러!\033[0m"
        continue
    fi

    if [ -f "${BASE}_in.txt" ] && [ -f "${BASE}_ans.txt" ]; then
        echo "[로컬 테스트] 2. ${BASE}_in.txt 입력값으로 실행 중..."
        ./local_test_run < "${BASE}_in.txt" > "local_out.txt"
        
        # 공백 무시하고(-w) 정답 파일과 비교
        diff -w "local_out.txt" "${BASE}_ans.txt" > /dev/null
        if [ $? -eq 0 ]; then
            echo -e "\033[32m[✅ 통과] 로컬 테스트 성공! (출력이 정답과 일치합니다)\033[0m"
        else
            echo -e "\033[31m[❌ 실패] 로컬 테스트 실패! (출력이 정답과 다릅니다)\033[0m"
            echo "================ 예상 정답 ================"
            cat "${BASE}_ans.txt"
            echo "================ 내 출력값 ================"
            cat "local_out.txt"
            
            # [추가 기능] 틀린 코드를 곧바로 수정할 수 있게 에디터 자동 연결
            echo ""
            read -p "🛠️ 곧바로 코드를 수정하시겠습니까? (y/n): " ans
            if [ "$ans" = "y" ]; then
                vi "$SRC"
            fi
        fi
        rm -f local_out.txt
    else
        echo "[⚠️ 경고] ${BASE}_in.txt 또는 ${BASE}_ans.txt 파일이 없어 비교를 생략합니다."
        echo "프로그램을 직접 실행합니다:"
        ./local_test_run
    fi

    rm -f local_test_run
done
