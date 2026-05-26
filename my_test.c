#include <stdio.h>

int main() {
    int a, b;
    if (scanf("%d %d", &a, &b) == 2) {
        printf("%d\n", a + b); // 두 수의 합 출력
    }
    return 0;
}
