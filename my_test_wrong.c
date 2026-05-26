#include <stdio.h>

int main() {
    int a, b;
    if (scanf("%d %d", &a, &b) == 2) {
        // [버그] 덧셈(+)을 해야 하는데 곱셈(*)을 해버린 학생 코드
        printf("%d\n", a + b); 
    }
    return 0;
}
