#include <stdio.h>

int main() {
    printf("선언되지 않은 변수 테스트입니다.\n");
    
    // 변수 'sum'은 위에서 int나 float로 선언된 적이 없습니다!
    // 서버가 "undeclared" 패턴을 감지해야 합니다.
   sum = 100 + 200;
    
    printf("결과: %d\n", sum);
    return 0;
}
