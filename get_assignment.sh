#!/bin/bash
# 학생이 과제를 처음 다운로드(Clone)받는 과정을 시뮬레이션하는 스크립트

echo "==========================================="
echo "📦 GitHub Classroom 과제 다운로드를 시작합니다..."
echo "==========================================="

# 학생용 작업 폴더 생성
STUDENT_DIR="student_workspace"
mkdir -p "$STUDENT_DIR"

# 필수 스크립트 및 실행 파일 복사 (마치 git clone으로 받아온 것처럼)
cp test.sh "$STUDENT_DIR/"
cp submit.sh "$STUDENT_DIR/"
cp client_bin "$STUDENT_DIR/" 2>/dev/null
cp inotify_watch.c "$STUDENT_DIR/" 2>/dev/null
cp inotify_watch "$STUDENT_DIR/" 2>/dev/null

# 교수님이 미리 준비해둔 과제 뼈대(Skeleton) 코드와 테스트 케이스 생성
cat << 'EOF' > "$STUDENT_DIR/hw1_calculator.c"
#include <stdio.h>

// [과제 1] 두 숫자를 더하는 프로그램을 작성하세요.
int main() {
    int a, b;
    // 여기에 코드를 작성하세요.
    
    return 0;
}
EOF

cat << 'EOF' > "$STUDENT_DIR/hw1_calculator_in.txt"
10 25
EOF

cat << 'EOF' > "$STUDENT_DIR/hw1_calculator_ans.txt"
35
EOF

# 스크립트에 실행 권한 부여
chmod +x "$STUDENT_DIR/test.sh" "$STUDENT_DIR/submit.sh"

echo "✅ 과제 다운로드가 완료되었습니다!"
echo ""
echo "👇 다음 명령어를 입력하여 과제를 시작하세요 👇"
echo "cd $STUDENT_DIR"
echo "vi hw1_calculator.c  (코드 작성)"
echo "./test.sh hw1_calculator.c  (로컬 테스트)"
echo "./submit.sh hw1_calculator.c  (서버로 최종 제출)"
