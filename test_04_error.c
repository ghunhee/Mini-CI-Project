#include <stdio.h>

int main() {
    printf("[테스트 4] 스마트 에러 점프 테스트\n");
    
    // ⚠️ 바로 아래 9번째 줄 끝에 세미콜론(;)을 고의로 뺐습니다!
    // 클라이언트에서 9번 줄로 vi 에디터가 열려야 합니다.
    int a = 100
    int b = 200;
    
    printf("결과: %d\n", a + b);
    return 0;
}
