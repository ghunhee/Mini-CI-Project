/*
 * inotify_watch.c
 * - 지정된 .c 파일들을 inotify로 감시
 * - 변경 감지 시 timerfd로 300ms 디바운싱
 * - 디바운싱 후 client_bin을 execvp로 재실행
 *
 * 빌드: gcc -o inotify_watch inotify_watch.c
 * 사용: ./inotify_watch <client_bin경로> <파일1.c> [파일2.c ...]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <sys/types.h>

#define MAX_FILES       64
#define EVENT_BUF_LEN   (10 * (sizeof(struct inotify_event) + 256))
#define DEBOUNCE_MS     300   /* 연속 저장 무시 시간 (ms) */
#define MAX_EVENTS      8

/* 감시 디스크립터 ↔ 파일경로 매핑 */
typedef struct {
    int  wd;
    char path[256];
} WatchEntry;

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

/* timerfd를 DEBOUNCE_MS 후 1회 발동하도록 (재)설정 */
static void arm_debounce(int tfd) {
    struct itimerspec ts = {
        .it_interval = {0, 0},                          /* 반복 없음 */
        .it_value    = {0, DEBOUNCE_MS * 1000000L},     /* 300ms 후 1회 */
    };
    if (timerfd_settime(tfd, 0, &ts, NULL) == -1) {
        perror("[watch] timerfd_settime");
    }
}

/* client_bin을 fork/exec으로 실행하고 종료까지 대기 */
static void run_client(char *client_bin, char **files, int num_files) {
    /* argv 구성: client_bin file1 file2 ... NULL */
    char **argv = malloc((num_files + 2) * sizeof(char *));
    if (!argv) { perror("malloc"); return; }

    argv[0] = client_bin;
    for (int i = 0; i < num_files; i++)
        argv[i + 1] = files[i];
    argv[num_files + 1] = NULL;

    pid_t pid = fork();
    if (pid == -1) {
        perror("[watch] fork");
        free(argv);
        return;
    }

    if (pid == 0) {
        /* 자식: client_bin 실행 */
        execvp(client_bin, argv);
        perror("[watch] execvp");
        _exit(127);
    }

    /* 부모: 자식 종료 대기 */
    int status;
    waitpid(pid, &status, 0);
    free(argv);
}

/* 현재 시각을 [HH:MM:SS] 형태로 출력 */
static void print_timestamp(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    printf("[%02d:%02d:%02d]", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "사용법: %s <client_bin> <파일1.c> [파일2.c ...]\n", argv[0]);
        return 1;
    }

    char *client_bin = argv[1];
    char **files     = &argv[2];
    int   num_files  = argc - 2;

    if (num_files > MAX_FILES) {
        fprintf(stderr, "[watch] 파일 수가 너무 많습니다 (최대 %d)\n", MAX_FILES);
        return 1;
    }

    /* SIGINT(Ctrl+C) 처리 */
    struct sigaction sa = { .sa_handler = handle_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* ── inotify 초기화 ── */
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd == -1) { perror("[watch] inotify_init1"); return 1; }

    WatchEntry watches[MAX_FILES];
    int nwatches = 0;

    for (int i = 0; i < num_files; i++) {
        /*
         * IN_CLOSE_WRITE : 파일이 쓰기 모드로 열렸다가 닫힐 때
         *                  (vim :w, vscode 저장 등 대부분의 에디터)
         * IN_MOVED_TO    : atomic save(임시파일 → rename) 방식 에디터 대응
         */
        int wd = inotify_add_watch(ifd, files[i],
                                   IN_CLOSE_WRITE | IN_MOVED_TO);
        if (wd == -1) {
            fprintf(stderr, "[watch] inotify_add_watch(%s): %s\n",
                    files[i], strerror(errno));
            continue;
        }
        watches[nwatches].wd = wd;
        strncpy(watches[nwatches].path, files[i],
                sizeof(watches[nwatches].path) - 1);
        nwatches++;
    }

    if (nwatches == 0) {
        fprintf(stderr, "[watch] 감시할 파일이 없습니다.\n");
        close(ifd);
        return 1;
    }

    /* ── timerfd 초기화 (디바운싱용, 처음엔 비활성) ── */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd == -1) { perror("[watch] timerfd_create"); close(ifd); return 1; }

    /* ── epoll 초기화 ── */
    int efd = epoll_create1(0);
    if (efd == -1) { perror("[watch] epoll_create1"); close(tfd); close(ifd); return 1; }

    struct epoll_event ev;

    ev.events  = EPOLLIN;
    ev.data.fd = ifd;
    epoll_ctl(efd, EPOLL_CTL_ADD, ifd, &ev);

    ev.events  = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(efd, EPOLL_CTL_ADD, tfd, &ev);

    /* ── 시작 메시지 ── */
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║          👁  Mini CI Watch Mode              ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  감시 파일 목록:                             ║\n");
    for (int i = 0; i < nwatches; i++)
        printf("║    • %-40s║\n", watches[i].path);
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  파일 저장 시 자동으로 채점이 시작됩니다.   ║\n");
    printf("║  종료: Ctrl+C                                ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    /* 시작 시 1회 즉시 채점 */
    print_timestamp();
    printf(" 초기 채점 시작...\n\n");
    fflush(stdout);
    run_client(client_bin, files, num_files);

    /* ── 메인 이벤트 루프 ── */
    struct epoll_event events[MAX_EVENTS];
    char changed_file[256] = {0};   /* 마지막으로 변경된 파일 이름 */

    while (g_running) {
        int n = epoll_wait(efd, events, MAX_EVENTS, 1000 /* 1초 타임아웃 */);
        if (n == -1) {
            if (errno == EINTR) continue;   /* 시그널로 인한 인터럽트 */
            perror("[watch] epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            /* ── inotify 이벤트: 파일 변경 감지 ── */
            if (fd == ifd) {
                char buf[EVENT_BUF_LEN] __attribute__((aligned(8)));
                ssize_t len = read(ifd, buf, sizeof(buf));
                if (len <= 0) continue;

                /* 변경된 파일 확인 후 wd로 경로 역조회 */
                char *ptr = buf;
                while (ptr < buf + len) {
                    struct inotify_event *ie = (struct inotify_event *)ptr;

                    /* wd → 파일 경로 역조회 */
                    for (int w = 0; w < nwatches; w++) {
                        if (watches[w].wd == ie->wd) {
                            strncpy(changed_file, watches[w].path,
                                    sizeof(changed_file) - 1);
                            break;
                        }
                    }
                    ptr += sizeof(struct inotify_event) + ie->len;
                }

                /* 디바운스 타이머 재설정 (연속 저장 무시) */
                arm_debounce(tfd);
            }

            /* ── timerfd 이벤트: 디바운스 완료 → 채점 실행 ── */
            if (fd == tfd) {
                /* timerfd 카운터 소비 (필수) */
                uint64_t expirations;
                ssize_t r = read(tfd, &expirations, sizeof(expirations));
                (void)r; /* timerfd 카운터 소비 - 반환값 불필요 */

                print_timestamp();
                if (changed_file[0])
                    printf(" \033[1;33m%s\033[0m 변경 감지 → 채점 시작...\n\n",
                           changed_file);
                else
                    printf(" 변경 감지 → 채점 시작...\n\n");
                fflush(stdout);

                run_client(client_bin, files, num_files);

                changed_file[0] = '\0';
            }
        }
    }

    /* ── 정리 ── */
    printf("\n[watch] 종료합니다.\n");
    for (int i = 0; i < nwatches; i++)
        inotify_rm_watch(ifd, watches[i].wd);
    close(efd);
    close(tfd);
    close(ifd);
    return 0;
}
