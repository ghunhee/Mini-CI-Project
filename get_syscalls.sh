#!/bin/bash
gcc -static test_ac.c -o test_ac
strace ./test_ac < server/test_cases/case1_in.txt 2> strace.log
echo "=== STRACE LOG ==="
cat strace.log | grep -v "="
