#include <stdio.h>
#include <sys/socket.h>
int main() {
    printf("Trying to connect external network...\n");
    // 네트워크 소켓을 여는 순간 eBPF/Seccomp가 감지하고 사살해야 함!
    int sock = socket(AF_INET, SOCK_STREAM, 0); 
    return 0;
}
