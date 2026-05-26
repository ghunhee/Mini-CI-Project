#include <stdio.h>

int main() {
    int a, b;
    scanf("%d %d", &a, &b);
    
    // 원래 정답은 "30" 이어야 하지만, 학생이 실수로 빈 칸과 엔터를 마구 넣었습니다!
    // 과거의 단순 memcmp 엔진이었다면 무조건 오답(WA) 처리되었을 코드입니다.
    printf("     %d    \n\n\n", a + b);
    
    return 0;
}
