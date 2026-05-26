#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("[테스트 2] 서버 격리(Sandboxing) 보안 테스트\n");
    
    // 1. 서버의 민감한 시스템 파일에 무단 쓰기/수정 시도
    FILE *f1 = fopen("/etc/passwd", "a");
    if (f1 == NULL) {
        printf("✅ 보안 테스트 1 통과: 시스템 파일 접근이 완벽히 차단되었습니다!\n");
    } else {
        printf("❌ 보안 경고: 시스템 파일 접근이 뚫렸습니다!\n");
        fclose(f1);
    }
    
    // 2. 루트 폴더 등 권한 없는 곳에 악성 파일 생성 시도
    FILE *f2 = fopen("/root/hacked.txt", "w");
    if (f2 == NULL) {
        printf("✅ 보안 테스트 2 통과: 무단 파일 생성이 완벽히 차단되었습니다!\n");
    } else {
        printf("❌ 보안 경고: 파일 생성이 뚫렸습니다!\n");
        fclose(f2);
    }

    return 0;
}
