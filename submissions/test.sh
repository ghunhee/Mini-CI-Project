#!/bin/bash
echo "이 스크립트는 1주차 채점용 스크립트입니다."
# 채점 로직 예시
gcc source.c -o a.out
./a.out < input.txt > output.txt
diff output.txt expected.txt
