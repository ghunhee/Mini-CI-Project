#include <stdio.h>

int main() {
    int a = 10;
    int b = 20;
    
    // if문의 조건 소괄호 ')' 가 닫히지 않았습니다!
    // 서버가 "expected ')'" 패턴을 감지해야 합니다.
    if (a < b {
        printf("a가 b보다 작습니다.\n");
    }
    
    return 0;
}
