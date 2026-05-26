include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    printf("Attempting to steal passwords...\n");
    // /etc/passwd 파일 무단 접근 시도 (openat 시스템 콜 발생 유도)
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd >= 0) {
        printf("Hacked file open success!\n");
        close(fd);
    } else {
        printf("Failed to open file.\n");
    }
    return 0;
}
