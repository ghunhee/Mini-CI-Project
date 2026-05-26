#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("[*] 악의적인 학생: 서버의 주요 파일을 탈취 시도합니다...\n");
    
    // 타겟 1: 정답 파일
    FILE *f1 = fopen("../test_cases/ans.txt", "r");
    if (f1) {
        printf("  -> 해킹 성공: 정답 파일을 열었습니다!\n");
        fclose(f1);
    }
    
    // 타겟 2: 리눅스 비밀번호 파일
    FILE *f2 = fopen("/etc/passwd", "r");
    if (f2) {
        printf("  -> 해킹 성공: /etc/passwd 파일을 열었습니다!\n");
        fclose(f2);
    }
    
    return 0;
}
