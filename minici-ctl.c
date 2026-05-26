/*
 * minici-ctl.c
 * Mini CI 서버 관리 CLI 도구
 *
 * 빌드: gcc -O2 -o minici-ctl minici-ctl.c
 *
 * 사용:
 *   minici-ctl status
 *   minici-ctl pause
 *   minici-ctl resume
 *   minici-ctl set-timeout 15
 *   minici-ctl list-queue
 *   minici-ctl help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define CTL_SOCK_PATH "/tmp/minici.sock"
#define RESP_BUF_SIZE 4096

static void usage(const char *prog) {
    fprintf(stderr,
        "사용법: %s <명령> [인자]\n"
        "\n"
        "명령 목록:\n"
        "  status               서버 전체 상태 조회\n"
        "  pause                신규 제출 일시 중단\n"
        "  resume               신규 제출 재개\n"
        "  set-timeout <초>     실행 제한 시간 변경 (1~300)\n"
        "  list-queue           대기 큐 길이 조회\n"
        "  help                 서버 측 도움말\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /* ── 명령어 조합 ── */
    char cmd[256] = {0};

    if (strcmp(argv[1], "set-timeout") == 0) {
        if (argc < 3) {
            fprintf(stderr, "[ERR] set-timeout 은 초 단위 인자가 필요합니다.\n"
                            "  예) minici-ctl set-timeout 15\n");
            return 1;
        }
        snprintf(cmd, sizeof(cmd), "set-timeout %s\n", argv[2]);
    } else {
        snprintf(cmd, sizeof(cmd), "%s\n", argv[1]);
    }

    /* ── UNIX Domain Socket 연결 ── */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[ERR] socket");
        return 1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == ENOENT || errno == ECONNREFUSED) {
            fprintf(stderr,
                "[ERR] 서버에 연결할 수 없습니다.\n"
                "      Mini CI 서버가 실행 중인지 확인하세요.\n");
        } else {
            perror("[ERR] connect");
        }
        close(fd);
        return 1;
    }

    /* ── 명령 전송 ── */
    if (send(fd, cmd, strlen(cmd), 0) < 0) {
        perror("[ERR] send");
        close(fd);
        return 1;
    }

    /* ── 응답 수신 및 출력 ── */
    char resp[RESP_BUF_SIZE];
    ssize_t n;

    while ((n = recv(fd, resp, sizeof(resp) - 1, 0)) > 0) {
        resp[n] = '\0';
        fputs(resp, stdout);
    }

    close(fd);
    return 0;
}
