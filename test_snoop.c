#include <stdio.h>
int main() {
    printf("Trying to steal /etc/passwd...\n");
    // 금지된 시스템 콜(openat)을 호출하는 순간 즉결 사살되어야 함!
    FILE *f = fopen("/etc/passwd", "r");
    if (f) {
        printf("HACKED!\n");
    }
    return 0;
}
