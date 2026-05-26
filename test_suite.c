/**
 * =============================================================================
 *  Mini CI Server — 통합 보안 테스트 스위트
 *  test_suite.c
 *
 *  테스트 레이어:
 *    Layer 0. 기본 동작 (정상 AC/WA/CE 판정)
 *    Layer 1. Seccomp 화이트리스트 위반
 *    Layer 2. eBPF 블랙리스트 탐지
 *    Layer 3. 자원 제한 (TLE / MLE)
 *    Layer 4. 네임스페이스 탈출 시도
 *    Layer 5. PID 1 탈취 / fork bomb
 *    Layer 6. 파일시스템 접근 제한
 *    Layer 7. 심볼릭 링크 공격 (work_dir 삭제 경로)
 * =============================================================================
 *
 *  빌드: gcc -O2 -o test_suite test_suite.c
 *  실행: ./test_suite [서버IP] [포트]   (기본값: 127.0.0.1 9000)
 *
 *  각 테스트는 독립적으로 서버에 소스코드를 전송하고,
 *  돌아온 ResultHeader.status 코드와 message로 통과/실패를 판정함.
 *
 *  예상 status 코드표:
 *    0 = AC / Execution Success
 *    1 = Compile Error
 *    2 = Wrong Answer
 *    3 = Time Limit Exceeded
 *    4 = Runtime Error
 *    5 = Security Violation (Seccomp / eBPF)
 * =============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <errno.h>

#include "include/protocol.h"

/* ── 색상 출력 ── */
#define CLR_RESET  "\033[0m"
#define CLR_RED    "\033[1;31m"
#define CLR_GREEN  "\033[1;32m"
#define CLR_YELLOW "\033[1;33m"
#define CLR_CYAN   "\033[1;36m"
#define CLR_BLUE   "\033[1;34m"
#define CLR_BOLD   "\033[1m"

/* ── 전역 카운터 ── */
static int g_pass = 0, g_fail = 0, g_total = 0;
static const char *g_server_ip   = "127.0.0.1";
static int         g_server_port = PORT;

/* ═══════════════════════════════════════════════════════
 *  헬퍼: 서버에 소스코드 전송 → ResultHeader 수신
 * ═══════════════════════════════════════════════════════ */
static int send_code(const char *name, const char *src, ResultHeader *out) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(g_server_port),
    };
    inet_pton(AF_INET, g_server_ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, CLR_RED "  [ERR] 서버 연결 실패 (%s:%d): %s\n" CLR_RESET,
                g_server_ip, g_server_port, strerror(errno));
        close(sock);
        return -1;
    }

    FileHeader fh;
    memset(&fh, 0, sizeof(fh));
    strncpy(fh.filename, name, MAX_FILENAME_LEN - 1);
    fh.file_size = (long)strlen(src);

    if (send(sock, &fh, sizeof(fh), 0) < 0) { close(sock); return -1; }
    if (send(sock, src, fh.file_size, 0) < 0) { close(sock); return -1; }

    memset(out, 0, sizeof(*out));
    ssize_t n = recv(sock, out, sizeof(*out), MSG_WAITALL);
    close(sock);
    return (n == sizeof(*out)) ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════
 *  테스트 러너
 *   expected_status: 기대하는 status 코드 (음수면 무시)
 *   check_msg:       message에 포함되어야 할 문자열 (NULL이면 무시)
 * ═══════════════════════════════════════════════════════ */
static void run_test(const char *test_name,
                     const char *src,
                     int         expected_status,
                     const char *check_msg)
{
    g_total++;
    printf(CLR_BOLD "\n[%02d] %s\n" CLR_RESET, g_total, test_name);
    printf("     기대 status: %d%s\n", expected_status,
           check_msg ? " | 메시지 포함 확인" : "");

    ResultHeader res;
    if (send_code(test_name, src, &res) < 0) {
        printf(CLR_RED "  ✗ FAIL — 서버 통신 오류\n" CLR_RESET);
        g_fail++;
        return;
    }

    printf("     실제 status: %d | 시간: %ld ms\n", res.status, res.time_ms);
    printf("     메시지: %s\n", res.message);

    int status_ok = (expected_status < 0) || (res.status == expected_status);
    int msg_ok    = (!check_msg) || (strstr(res.message, check_msg) != NULL);

    if (status_ok && msg_ok) {
        printf(CLR_GREEN "  ✓ PASS\n" CLR_RESET);
        g_pass++;
    } else {
        printf(CLR_RED "  ✗ FAIL");
        if (!status_ok) printf(" (status: 기대=%d 실제=%d)", expected_status, res.status);
        if (!msg_ok)    printf(" (메시지에 '%s' 없음)", check_msg);
        printf("\n" CLR_RESET);
        g_fail++;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Layer 0 — 기본 동작 테스트
 *  목적: AC / WA / CE가 정상적으로 판별되는지 확인
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 0-1: Hello World → expected.txt와 일치하면 AC */
static const char SRC_HELLO[] =
    "#include <stdio.h>\n"
    "int main() { printf(\"Hello, World!\\n\"); return 0; }\n";

/* 0-2: 컴파일 에러 */
static const char SRC_CE[] =
    "int main() { this_is_not_valid_c_code }\n";

/* 0-3: 정상 실행이지만 출력이 틀린 경우 (WA) */
static const char SRC_WA[] =
    "#include <stdio.h>\n"
    "int main() { printf(\"WRONG_ANSWER_XYZ_12345\\n\"); return 0; }\n";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Layer 1 — Seccomp 화이트리스트 위반
 *  목적: 허용되지 않은 syscall 호출 시 SIGSYS(status=5) 반환 확인
 *  각 테스트는 화이트리스트에 없는 syscall을 직접 호출함.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 1-1: socket() 직접 호출 — 네트워크 차단 */
static const char SRC_SOCKET[] =
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* AF_INET SOCK_STREAM: 외부 연결 시도 */\n"
    "    long r = syscall(41, 2, 1, 0);  /* SYS_socket */\n"
    "    return (int)r;\n"
    "}\n";

/* 1-2: openat() — 파일시스템 읽기 시도 */
static const char SRC_OPENAT[] =
    "#include <sys/syscall.h>\n"
    "#include <fcntl.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* /etc/passwd 읽기 시도 */\n"
    "    long r = syscall(257, -100, \"/etc/passwd\", 0, 0); /* SYS_openat */\n"
    "    return (int)r;\n"
    "}\n";

/* 1-3: execve() — 쉘 실행 시도 (seccomp 화이트리스트에는 있지만 eBPF 블랙리스트) */
static const char SRC_EXECVE[] =
    "#include <unistd.h>\n"
    "int main() {\n"
    "    char *argv[] = {\"/bin/sh\", \"-c\", \"id\", NULL};\n"
    "    char *envp[] = {NULL};\n"
    "    execve(\"/bin/sh\", argv, envp);\n"
    "    return 0;\n"
    "}\n";

/* 1-4: clone() — 새 프로세스/스레드 생성 시도 */
static const char SRC_CLONE[] =
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "#include <sched.h>\n"
    "int main() {\n"
    "    /* clone(SIGCHLD, 0) — 자식 프로세스 생성 */\n"
    "    long r = syscall(56, 17, 0, 0, 0, 0); /* SYS_clone */\n"
    "    return (int)r;\n"
    "}\n";

/* 1-5: ptrace() — 다른 프로세스 디버깅/탈취 */
static const char SRC_PTRACE[] =
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* PTRACE_TRACEME */\n"
    "    long r = syscall(101, 0, 0, 0, 0); /* SYS_ptrace */\n"
    "    return (int)r;\n"
    "}\n";

/* 1-6: mknod() — 디바이스 파일 생성 */
static const char SRC_MKNOD[] =
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    long r = syscall(133, \"/tmp/evil_dev\", 0x2000 | 0600, 0); /* SYS_mknod */\n"
    "    return (int)r;\n"
    "}\n";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Layer 2 — eBPF 탐지 (blocked_syscalls 맵)
 *  목적: eBPF가 먼저 잡아내는 syscall 패턴 확인
 *  (Seccomp와 eBPF 양쪽 다 막혀 있는 케이스 — status=5)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 2-1: connect() — eBPF 블랙리스트 42번 */
static const char SRC_CONNECT[] =
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* connect syscall (42) — eBPF 즉결사살 대상 */\n"
    "    long r = syscall(42, 0, 0, 0);\n"
    "    return (int)r;\n"
    "}\n";

/* 2-2: fork() → execve() 체이닝 — 두 단계 우회 시도 */
static const char SRC_FORK_EXEC[] =
    "#include <unistd.h>\n"
    "#include <sys/types.h>\n"
    "int main() {\n"
    "    /* fork(57)는 eBPF 블랙리스트 — 자식에서 쉘 실행 시도 */\n"
    "    pid_t p = fork();\n"
    "    if (p == 0) {\n"
    "        execl(\"/bin/sh\", \"sh\", \"-c\", \"cat /etc/shadow\", NULL);\n"
    "    }\n"
    "    return 0;\n"
    "}\n";

/* 2-3: io_uring_setup() — 최신 우회 벡터 (syscall 425) */
static const char SRC_IO_URING[] =
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* io_uring_setup(425) — 비동기 I/O 인터페이스로 파일/네트워크 우회 시도 */\n"
    "    long r = syscall(425, 8, 0);\n"
    "    return (int)r;\n"
    "}\n";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Layer 3 — 자원 제한 테스트
 *  목적: CPU/메모리/파일 크기 제한이 실제로 작동하는지 확인
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 3-1: 무한 루프 — TLE (RLIMIT_CPU) */
static const char SRC_TLE[] =
    "int main() {\n"
    "    volatile long x = 0;\n"
    "    while (1) x++;  /* 무한 루프 */\n"
    "    return 0;\n"
    "}\n";

/* 3-2: 과도한 메모리 할당 — MLE (RLIMIT_AS = 64MB) */
static const char SRC_MLE[] =
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "int main() {\n"
    "    /* 1GB 할당 시도 — RLIMIT_AS(64MB) 초과 */\n"
    "    size_t sz = 1024UL * 1024 * 1024;\n"
    "    char *p = malloc(sz);\n"
    "    if (p) memset(p, 0, sz);  /* 실제 물리 메모리 강제 할당 */\n"
    "    return 0;\n"
    "}\n";

/* 3-3: 대용량 파일 출력 — FSIZE 제한 (RLIMIT_FSIZE = 1MB) */
static const char SRC_FSIZE[] =
    "#include <stdio.h>\n"
    "#include <string.h>\n"
    "int main() {\n"
    "    /* 10MB 출력 시도 — RLIMIT_FSIZE(1MB) 초과 → SIGXFSZ */\n"
    "    char buf[1024];\n"
    "    memset(buf, 'A', sizeof(buf));\n"
    "    for (int i = 0; i < 10 * 1024; i++)\n"
    "        fwrite(buf, 1, sizeof(buf), stdout);\n"
    "    return 0;\n"
    "}\n";

/* 3-4: Fork Bomb — RLIMIT_NPROC=1 으로 차단되어야 함 */
static const char SRC_FORK_BOMB[] =
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* fork bomb: :(){ :|:& };: — RLIMIT_NPROC(1)으로 차단 */\n"
    "    /* + eBPF/seccomp의 fork 차단으로 이중 방어 */\n"
    "    while (fork() >= 0);  /* 끊임없이 자식 프로세스 생성 */\n"
    "    return 0;\n"
    "}\n";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Layer 4 — 네임스페이스 탈출 시도
 *  목적: unshare()된 환경에서 탈출 불가능함을 확인
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 4-1: /proc/self/ns 접근으로 네임스페이스 정보 열람 시도 */
static const char SRC_NS_PROBE[] =
    "#include <stdio.h>\n"
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* openat 차단으로 /proc/self/ns/* 접근 불가해야 함 */\n"
    "    long fd = syscall(257, -100, \"/proc/self/ns/net\", 0, 0);\n"
    "    printf(\"ns_fd=%ld\\n\", fd);\n"
    "    return 0;\n"
    "}\n";

/* 4-2: unshare() 재시도 — 새 네임스페이스 생성으로 탈출 시도 */
static const char SRC_UNSHARE[] =
    "#include <sys/syscall.h>\n"
    "#include <sched.h>\n"
    "#include <unistd.h>\n"
    "#include <stdio.h>\n"
    "int main() {\n"
    "    /* unshare(272) — 새 네임스페이스 생성으로 격리 탈출 시도 */\n"
    "    long r = syscall(272, 0x20000000); /* CLONE_NEWUSER */\n"
    "    printf(\"unshare result: %ld\\n\", r);\n"
    "    return 0;\n"
    "}\n";

/* 4-3: setns() — 다른 네임스페이스로 참여 시도 */
static const char SRC_SETNS[] =
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* setns(308) */\n"
    "    long r = syscall(308, 0, 0);\n"
    "    return (int)r;\n"
    "}\n";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Layer 5 — 권한 상승 시도
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 5-1: setuid(0) — root 탈취 시도 */
static const char SRC_SETUID[] =
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "#include <stdio.h>\n"
    "int main() {\n"
    "    long r = syscall(105, 0); /* SYS_setuid */\n"
    "    printf(\"setuid(0) = %ld, uid = %d\\n\", r, (int)getuid());\n"
    "    return 0;\n"
    "}\n";

/* 5-2: capset() — 권한 재취득 시도 */
static const char SRC_CAPSET[] =
    "#include <sys/syscall.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* capset(126) */\n"
    "    long r = syscall(126, 0, 0);\n"
    "    return (int)r;\n"
    "}\n";

/* 5-3: prctl(PR_SET_SECCOMP) — seccomp 재설정 우회 시도 */
static const char SRC_PRCTL_SECCOMP[] =
    "#include <sys/syscall.h>\n"
    "#include <sys/prctl.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* prctl을 이용해 seccomp 필터 비활성화 시도 */\n"
    "    /* PR_SET_NO_NEW_PRIVS = 38 로 인해 실패해야 함 */\n"
    "    long r = syscall(157, 22, 0, 0, 0, 0); /* SYS_prctl, PR_SET_SECCOMP=22 */\n"
    "    return (int)r;\n"
    "}\n";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Layer 6 — 파일시스템 공격
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 6-1: /etc/passwd 직접 읽기 */
static const char SRC_READ_PASSWD[] =
    "#include <stdio.h>\n"
    "int main() {\n"
    "    /* fopen은 내부적으로 openat syscall 사용 → 차단 */\n"
    "    FILE *f = fopen(\"/etc/passwd\", \"r\");\n"
    "    if (f) {\n"
    "        char buf[256];\n"
    "        while (fgets(buf, sizeof(buf), f)) printf(\"%s\", buf);\n"
    "        fclose(f);\n"
    "    } else {\n"
    "        printf(\"blocked\\n\");\n"
    "    }\n"
    "    return 0;\n"
    "}\n";

/* 6-2: /proc/1/maps 읽기 — PID 1(init) 메모리 레이아웃 정찰 */
static const char SRC_PROC_MAPS[] =
    "#include <stdio.h>\n"
    "int main() {\n"
    "    FILE *f = fopen(\"/proc/1/maps\", \"r\");\n"
    "    char buf[256];\n"
    "    if (f) {\n"
    "        fgets(buf, sizeof(buf), f);\n"
    "        printf(\"%s\", buf);\n"
    "        fclose(f);\n"
    "    }\n"
    "    return 0;\n"
    "}\n";

/* 6-3: /tmp에 파일 쓰기 시도 */
static const char SRC_WRITE_TMP[] =
    "#include <stdio.h>\n"
    "#include <sys/syscall.h>\n"
    "#include <fcntl.h>\n"
    "#include <unistd.h>\n"
    "int main() {\n"
    "    /* openat으로 /tmp/evil 파일 생성 시도 */\n"
    "    long fd = syscall(257, -100, \"/tmp/evil_payload\",\n"
    "                      0x241, 0644); /* O_WRONLY|O_CREAT|O_TRUNC */\n"
    "    if (fd >= 0) {\n"
    "        syscall(1, fd, \"pwned\", 5); /* write */\n"
    "    }\n"
    "    return 0;\n"
    "}\n";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Layer 7 — 정상 동작 확인 (회귀 테스트)
 *  목적: 보안 레이어가 정상 코드를 방해하지 않는지 확인
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 7-1: 표준 입출력 */
static const char SRC_STDIO[] =
    "#include <stdio.h>\n"
    "int main() {\n"
    "    int a, b;\n"
    "    if (scanf(\"%d %d\", &a, &b) == 2)\n"
    "        printf(\"%d\\n\", a + b);\n"
    "    return 0;\n"
    "}\n";

/* 7-2: 동적 메모리 할당 (허용 범위 내) */
static const char SRC_MALLOC[] =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "int main() {\n"
    "    /* 1MB 할당 — RLIMIT_AS(64MB) 이내 */\n"
    "    char *p = malloc(1024 * 1024);\n"
    "    if (!p) { printf(\"alloc_fail\\n\"); return 1; }\n"
    "    memset(p, 42, 1024 * 1024);\n"
    "    printf(\"ok %d\\n\", p[512]);\n"
    "    free(p);\n"
    "    return 0;\n"
    "}\n";

/* 7-3: 수학 연산 집약적 코드 */
static const char SRC_COMPUTE[] =
    "#include <stdio.h>\n"
    "int main() {\n"
    "    long sum = 0;\n"
    "    for (long i = 1; i <= 1000000LL; i++) sum += i;\n"
    "    printf(\"%ld\\n\", sum);\n"
    "    return 0;\n"
    "}\n";

/* ═══════════════════════════════════════════════════════════════════════════
 *  메인
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (argc >= 2) g_server_ip   = argv[1];
    if (argc >= 3) g_server_port = atoi(argv[2]);

    printf(CLR_BOLD CLR_BLUE
           "\n╔══════════════════════════════════════════════════════╗\n"
           "║   Mini CI Server — 보안 통합 테스트 스위트 v1.0      ║\n"
           "║   대상: %s:%d%-*s║\n"
           "╚══════════════════════════════════════════════════════╝\n"
           CLR_RESET,
           g_server_ip, g_server_port,
           (int)(26 - strlen(g_server_ip)), "");

    /* ─── Layer 0: 기본 동작 ─────────────────────────────── */
    printf(CLR_CYAN "\n▶ Layer 0 — 기본 판정 동작\n" CLR_RESET);
    run_test("0-1_Hello_AC",     SRC_HELLO,   0,  NULL);   /* expected.txt와 맞으면 AC */
    run_test("0-2_Compile_Error", SRC_CE,     1,  NULL);   /* CE */
    run_test("0-3_Wrong_Answer",  SRC_WA,     2,  NULL);   /* WA */

    /* ─── Layer 1: Seccomp ───────────────────────────────── */
    printf(CLR_CYAN "\n▶ Layer 1 — Seccomp 화이트리스트 위반\n" CLR_RESET);
    run_test("1-1_socket_syscall",   SRC_SOCKET,  5, "Security");
    run_test("1-2_openat_etc_passwd",SRC_OPENAT,  5, "Security");
    run_test("1-3_execve_shell",     SRC_EXECVE,  5, "Security");
    run_test("1-4_clone_process",    SRC_CLONE,   5, "Security");
    run_test("1-5_ptrace",           SRC_PTRACE,  5, "Security");
    run_test("1-6_mknod",            SRC_MKNOD,   5, "Security");

    /* ─── Layer 2: eBPF ──────────────────────────────────── */
    printf(CLR_CYAN "\n▶ Layer 2 — eBPF 블랙리스트 탐지\n" CLR_RESET);
    run_test("2-1_connect_syscall",  SRC_CONNECT,    5, "Security");
    run_test("2-2_fork_exec_chain",  SRC_FORK_EXEC,  5, "Security");
    run_test("2-3_io_uring",         SRC_IO_URING,   5, "Security");

    /* ─── Layer 3: 자원 제한 ─────────────────────────────── */
    printf(CLR_CYAN "\n▶ Layer 3 — 자원 제한 (TLE / MLE / FSIZE)\n" CLR_RESET);
    run_test("3-1_Infinite_Loop_TLE",    SRC_TLE,        3, "Timeout");
    run_test("3-2_1GB_Malloc_MLE",       SRC_MLE,        4, NULL);     /* SIGSEGV or killed */
    run_test("3-3_10MB_Output_FSIZE",    SRC_FSIZE,      4, NULL);     /* SIGXFSZ */
    run_test("3-4_Fork_Bomb",            SRC_FORK_BOMB,  5, "Security");

    /* ─── Layer 4: 네임스페이스 탈출 ────────────────────── */
    printf(CLR_CYAN "\n▶ Layer 4 — 네임스페이스 탈출 시도\n" CLR_RESET);
    run_test("4-1_NS_Proc_Probe",  SRC_NS_PROBE, 5, "Security");
    run_test("4-2_Unshare_NewUser",SRC_UNSHARE,  5, "Security");
    run_test("4-3_Setns",         SRC_SETNS,    5, "Security");

    /* ─── Layer 5: 권한 상승 ─────────────────────────────── */
    printf(CLR_CYAN "\n▶ Layer 5 — 권한 상승 시도\n" CLR_RESET);
    run_test("5-1_setuid_root",      SRC_SETUID,        5, "Security");
    run_test("5-2_capset",           SRC_CAPSET,        5, "Security");
    run_test("5-3_prctl_seccomp",    SRC_PRCTL_SECCOMP, 5, "Security");

    /* ─── Layer 6: 파일시스템 공격 ──────────────────────── */
    printf(CLR_CYAN "\n▶ Layer 6 — 파일시스템 공격\n" CLR_RESET);
    run_test("6-1_Read_etc_passwd",  SRC_READ_PASSWD, 5, "Security");
    run_test("6-2_Read_proc_maps",   SRC_PROC_MAPS,   5, "Security");
    run_test("6-3_Write_tmp_evil",   SRC_WRITE_TMP,   5, "Security");

    /* ─── Layer 7: 정상 코드 회귀 테스트 ────────────────── */
    printf(CLR_CYAN "\n▶ Layer 7 — 정상 코드 회귀 테스트 (False Positive 검사)\n" CLR_RESET);
    run_test("7-1_StdIO_OK",   SRC_STDIO,   0, NULL);
    run_test("7-2_Malloc_OK",  SRC_MALLOC,  0, NULL);
    run_test("7-3_Compute_OK", SRC_COMPUTE, 0, NULL);

    /* ─── 최종 결과 ──────────────────────────────────────── */
    printf(CLR_BOLD
           "\n╔══════════════════════════════════╗\n"
           "║         테스트 결과 요약          ║\n"
           "╠══════════════════════════════════╣\n"
           "║  전체: %-3d  통과: %-3d  실패: %-3d ║\n"
           "╚══════════════════════════════════╝\n" CLR_RESET,
           g_total, g_pass, g_fail);

    if (g_fail == 0) {
        printf(CLR_GREEN CLR_BOLD "  ✓ 모든 보안 레이어가 정상 작동합니다!\n" CLR_RESET);
    } else {
        printf(CLR_RED CLR_BOLD "  ✗ %d개 테스트 실패 — 보안 레이어 점검 필요!\n" CLR_RESET, g_fail);
    }
    printf("\n");

    return (g_fail > 0) ? 1 : 0;
}
