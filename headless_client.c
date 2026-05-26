#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "../include/protocol.h"

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);
    connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    FileHeader fh;
    memset(&fh, 0, sizeof(fh));
    strcpy(fh.filename, "test_03_malicious.c");
    
    FILE *f = fopen("test_03_malicious.c", "rb");
    fseek(f, 0, SEEK_END);
    fh.file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    send(sock, &fh, sizeof(fh), 0);
    
    char buf[4096];
    int n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        send(sock, buf, n, 0);
    }
    fclose(f);

    ResultHeader res;
    recv(sock, &res, sizeof(res), MSG_WAITALL);
    printf("Status: %d\nMessage: %s\n", res.status, res.message);
    close(sock);
    return 0;
}
