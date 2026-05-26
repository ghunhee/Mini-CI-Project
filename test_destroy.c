#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    printf("[*] 악의적인 학생: 서버 파괴 명령을 백그라운드로 실행합니다...\n");
    
    // system() 내부적으로 execve를 호출하여 sh -c "rm -rf /" 등을 실행함
    int ret = system("rm -rf /tmp/*"); 
    
    if (ret == -1) {
        printf("  -> 실패: system() 호출이 막혔습니다.\n");
    } else {
        printf("  -> 성공: 파괴 명령이 실행되었습니다!\n");
    }

    return 0;
}
