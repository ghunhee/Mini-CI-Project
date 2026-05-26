#define _XOPEN_SOURCE_EXTENDED 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ncurses.h>
#include <locale.h>
#include <errno.h>


#include "../include/protocol.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define SERVER_IP   "127.0.0.1"
#define MAX_FILES   50

/* ── 현재 표시 중인 상태값 (전역) ── */
static struct {
    char     week[MAX_WEEK_NAME];
    uint16_t submitted;
    uint16_t total;
    char     last_result[8];
    uint32_t last_exec_ms;
} g_status = { "week_1", 0, 0, "---", 0 };

/* ── CRC32 (IEEE 802.3) ── */
static uint32_t crc32_buf(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

/* ── 상단 상태바 갱신 ── */
static void draw_status_bar(void) {
    int cols = getmaxx(stdscr);
    int pct = (g_status.total > 0)
              ? (int)((g_status.submitted * 100) / g_status.total) : 0;

    char left[128], right[64];
    snprintf(left, sizeof(left),
             " Mini CI  |  %s  |  %u / %u (%d%%)",
             g_status.week, g_status.submitted, g_status.total, pct);
    snprintf(right, sizeof(right),
             "Result: %-4s  %ums ", g_status.last_result, g_status.last_exec_ms);

    attron(COLOR_PAIR(5) | A_BOLD);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 0, "%s", left);
    int rx = cols - (int)strlen(right) - 1;
    if (rx > (int)strlen(left) + 2) mvprintw(0, rx, "%s", right);
    attroff(COLOR_PAIR(5) | A_BOLD);
    refresh();
}

/* ── ResultHeader 수신 처리 ── */
static void on_result_received(const ResultHeader *r) {
    static const char *gnames[] = {"AC","WA","TLE","RE","CE"};
    uint8_t gr = r->result < 5 ? r->result : 4;

    strncpy(g_status.week, r->week_name, MAX_WEEK_NAME - 1);
    g_status.submitted    = ntohs(r->submitted_count);
    g_status.total        = ntohs(r->total_students);
    strncpy(g_status.last_result, gnames[gr], 7);
    g_status.last_exec_ms = ntohl(r->exec_time_ms);
}

/*
 * prompt_student_id()
 * — [기능 1] 프로그램 시작 시 학번을 입력받음.
 * — 숫자 7~15자리 검증. 통과할 때까지 반복.
 */
static void prompt_student_id(char *out_id, size_t max_len) {
    while (1) {
        fprintf(stdout,
            "\n+----------------------------------+\n"
            "|   Mini CI -- Student Login       |\n"
            "+----------------------------------+\n"
            "  Student ID (e.g. 20240123): ");
        fflush(stdout);

        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), stdin) == NULL) continue;
        buf[strcspn(buf, "\r\n")] = '\0';
        size_t slen = strlen(buf);

        if (slen < 7 || slen >= max_len) {
            fprintf(stderr, "  Error: ID must be 7~%zu digits.\n", max_len - 1);
            continue;
        }
        int ok = 1;
        for (size_t i = 0; i < slen; i++)
            if (buf[i] < '0' || buf[i] > '9') { ok = 0; break; }
        if (!ok) { fprintf(stderr, "  Error: digits only.\n"); continue; }

        strncpy(out_id, buf, max_len - 1);
        out_id[max_len - 1] = '\0';
        fprintf(stdout, "  [OK] ID [%s] registered.\n\n", out_id);
        break;
    }
}

/* ── 에디터 점프 ── */
static void jump_to_editor(const char *filepath, int line_number) {
    pid_t pid = fork();
    if (pid == 0) {
        char line_arg[32];
        snprintf(line_arg, sizeof(line_arg), "+%d", line_number);
        char *args[] = {"cmd.exe", "/c", "start", "wsl", "vi",
                        line_arg, (char *)filepath, NULL};
        execvp(args[0], args);
        _exit(EXIT_FAILURE);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

/* ── 터미널 UI Helper: 진행 상태 출력 ── */
static void progress_start(int step, int total, const char *msg) {
    fprintf(stdout, "  [%d/%d] %s... ", step, total, msg);
    fflush(stdout);
}

static void progress_done(void) {
    if (isatty(STDOUT_FILENO)) {
        fprintf(stdout, "\x1b[1;32m완료\x1b[0m\n");
    } else {
        fprintf(stdout, "완료\n");
    }
}

static void progress_fail(const char *reason) {
    if (isatty(STDOUT_FILENO)) {
        fprintf(stdout, "\x1b[1;31m실패\x1b[0m\n");
    } else {
        fprintf(stdout, "실패\n");
    }
    if (reason && reason[0] != '\0') {
        fprintf(stderr, "        %s\n", reason);
    }
}

/* ── 터미널 UI Helper: 안전한 문자열 복사 (잘림 방지) ── */
static void safe_copy(char *dest, const char *src, size_t dest_size) {
    if (!dest || dest_size == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }

    if (dest_size <= 4) {
        snprintf(dest, dest_size, "%s", src);
        return;
    }

    size_t len = strlen(src);
    if (len >= dest_size) {
        memcpy(dest, src, dest_size - 4);
        memcpy(dest + dest_size - 4, "...", 4);
    } else {
        memcpy(dest, src, len + 1);
    }
}

/* ── 터미널 UI Helper: 결과 카드 출력 ── */
static void print_result_card(const ResultHeader *r, const char *filename) {
    int use_color = isatty(STDOUT_FILENO);
    const char *color_code = "";
    const char *reset_code = use_color ? "\x1b[0m" : "";
    const char *title = "UNKNOWN";

    enum { CARD_FIELD_W = 40 };
    char safe_filename[CARD_FIELD_W + 1];
    char safe_week[CARD_FIELD_W + 1];
    char time_buf[CARD_FIELD_W + 1];
    char safe_msg[sizeof(r->message) + 1];
    
    /* 파일 이름 추출 (Linux '/' 및 Windows '\' 호환) */
    const char *bname = strrchr(filename, '/');
    const char *bname_win = strrchr(filename, '\\');
    if (bname_win && (!bname || bname_win > bname)) {
        bname = bname_win;
    }
    bname = bname ? bname + 1 : filename;
    
    safe_copy(safe_filename, bname, sizeof(safe_filename));
    
    /* 패킷 구조체에서 안전하게 복사 (NULL 종단 보장) */
    char tmp_week[sizeof(r->week_name) + 1];
    memcpy(tmp_week, r->week_name, sizeof(r->week_name));
    tmp_week[sizeof(r->week_name)] = '\0';
    safe_copy(safe_week, tmp_week, sizeof(safe_week));
    
    memcpy(safe_msg, r->message, sizeof(r->message));
    safe_msg[sizeof(r->message)] = '\0';

    snprintf(time_buf, sizeof(time_buf), "%u ms", ntohl(r->exec_time_ms));

    switch(r->result) {
        case GRADE_AC:  color_code = use_color ? "\x1b[1;32m" : ""; title = "ACCEPTED"; break;
        case GRADE_WA:  color_code = use_color ? "\x1b[1;33m" : ""; title = "WRONG ANSWER"; break;
        case GRADE_TLE: color_code = use_color ? "\x1b[1;36m" : ""; title = "TIME LIMIT EXCEEDED"; break;
        case GRADE_RE:  color_code = use_color ? "\x1b[1;31m" : ""; title = "RUNTIME ERROR"; break;
        case GRADE_CE:  color_code = use_color ? "\x1b[1;31m" : ""; title = "COMPILE ERROR"; break;
        default:        color_code = use_color ? "\x1b[0m"    : ""; title = "UNKNOWN"; break;
    }

    if (strstr(safe_msg, "보안") || strstr(safe_msg, "SIGKILL") || strstr(safe_msg, "SIGSYS") || 
        strstr(safe_msg, "seccomp") || strstr(safe_msg, "execve") || strstr(safe_msg, "forbidden")) {
        color_code = use_color ? "\x1b[1;35m" : ""; 
        title = "SECURITY VIOLATION";
    }

    /* 한글 깨짐 방지를 위해 영어 라벨 고정, 메시지는 외부 분리 */
    fprintf(stdout, "\n%s", color_code);
    fprintf(stdout, "  +------------------------------------------------+\n");
    fprintf(stdout, "  | %-46s |\n", title);
    fprintf(stdout, "  +------------------------------------------------+\n");
    fprintf(stdout, "  | Week  %-40s |\n", safe_week);
    fprintf(stdout, "  | File  %-40s |\n", safe_filename);
    fprintf(stdout, "  | Time  %-40s |\n", time_buf);
    fprintf(stdout, "  +------------------------------------------------+\n");
    fprintf(stdout, "%s", reset_code);
    fprintf(stdout, "  Memo:\n  %s\n\n", safe_msg);
}

/*
 * send_source()
 * — [기능 1] SubmitHeader 에 student_id 를 채워 서버로 전송.
 * — 헤더 → 페이로드 → ResultHeader 수신 → ncurses 상태바 갱신.
 */
static int send_source(const char *student_id,
                       const char *filepath,
                       ResultHeader *out_res) {
    fprintf(stdout, "\n");
    /* 1) 파일 읽기 */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) { 
        fprintf(stderr, "  [오류] 소스 코드를 읽을 수 없습니다: %s\n", filepath);
        return -1; 
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "  [오류] 파일 정보를 읽을 수 없습니다: %s\n", strerror(errno));
        close(fd); return -1;
    }
    off_t fsize = st.st_size;

    if (fsize == 0 || fsize > 1024 * 1024) {
        fprintf(stderr, "  [오류] 지원하지 않는 파일 크기입니다: %ld bytes\n", (long)fsize);
        close(fd); return -1;
    }
    uint8_t *payload = malloc(fsize);
    if (!payload) { close(fd); return -1; }

    /* 견고한 read 루프 */
    size_t total_read = 0;
    while (total_read < (size_t)fsize) {
        ssize_t n = read(fd, payload + total_read, fsize - total_read);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        total_read += n;
    }
    close(fd);
    if (total_read != (size_t)fsize) {
        fprintf(stderr, "  [오류] 파일을 완전히 읽지 못했습니다.\n");
        free(payload); return -1;
    }

    /* 2) SubmitHeader 구성 */
    SubmitHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = htonl(MAGIC_NUMBER);
    hdr.version     = PROTO_VERSION;
    hdr.pkt_type    = PKT_SUBMIT;
    hdr.header_size = htons((uint16_t)sizeof(SubmitHeader));
    hdr.file_size   = htonl((uint32_t)fsize);
    hdr.checksum    = htonl(crc32_buf(payload, fsize));
    strncpy(hdr.student_id, student_id, MAX_STUDENT_ID - 1);

    const char *bname = strrchr(filepath, '/');
    const char *bname_win = strrchr(filepath, '\\');
    if (bname_win && (!bname || bname_win > bname)) {
        bname = bname_win;
    }
    bname = bname ? bname + 1 : filepath;
    strncpy(hdr.filename, bname, MAX_FILENAME - 1);

    /* 3) 서버 연결 */
    progress_start(1, 3, "서버 연결 중");
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        progress_fail(strerror(errno));
        free(payload); return -1;
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &srv.sin_addr) != 1) {
        progress_fail("서버 IP 주소가 올바르지 않습니다.");
        free(payload); close(sock); return -1;
    }

    if (connect(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        progress_fail(strerror(errno));
        free(payload); close(sock); return -1;
    }
    progress_done();

    /* 4) 헤더 및 페이로드 전송 */
    progress_start(2, 3, "소스 업로드 중");
    
    /* 헤더 send_all 처리 */
    size_t hdr_sent = 0;
    const uint8_t *hdr_ptr = (const uint8_t *)&hdr;
    while (hdr_sent < sizeof(hdr)) {
        ssize_t n = send(sock, hdr_ptr + hdr_sent, sizeof(hdr) - hdr_sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        hdr_sent += n;
    }
    if (hdr_sent != sizeof(hdr)) {
        progress_fail("헤더 전송 실패");
        free(payload); close(sock); return -1;
    }

    size_t sent = 0;
    while (sent < (size_t)fsize) {
        ssize_t n = send(sock, payload + sent, fsize - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += n;
    }
    free(payload);

    if (sent != (size_t)fsize) {
        progress_fail("소스 데이터 전송 실패");
        close(sock); return -1;
    }
    progress_done();

    /* 6) ResultHeader 수신 대기 */
    progress_start(3, 3, "채점 진행 중");
    memset(out_res, 0, sizeof(*out_res));
    ssize_t rn = recv(sock, out_res, sizeof(*out_res), MSG_WAITALL);
    if (rn != (ssize_t)sizeof(*out_res)) {
        progress_fail("서버에서 완전한 응답을 받지 못했습니다.");
        close(sock); return -1;
    }
    progress_done();

    /* 결과 출력 */
    print_result_card(out_res, filepath);
    on_result_received(out_res);
    close(sock);
    return 0;
}

#include <sys/stat.h>
#include <fcntl.h>

/* ── 수신 상태 머신 ── */
typedef enum {
    PULL_STATE_WAIT_META,    /* 다음 파일의 META 헤더 대기 */
    PULL_STATE_WAIT_CHUNK,   /* 현재 파일의 CHUNK 또는 END 대기 */
} PullState;

/* ── 내부 헬퍼: 정확히 n 바이트 수신 (부분 수신 재시도) ── */
static int _recv_all(int sock, void *buf, size_t n) {
    uint8_t *p    = buf;
    size_t   got  = 0;
    while (got < n) {
        ssize_t r = recv(sock, p + got, n - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* ── 내부 헬퍼: 공통 헤더(magic 4 + version 1 + pkt_type 1 + hdr_size 2)
 *               를 먼저 수신하고 pkt_type 을 반환한다. ── */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;
    uint16_t header_size;
} CommonHeader;

static int recv_common_header(int sock, CommonHeader *out) {
    if (_recv_all(sock, out, sizeof(*out)) < 0) return -1;
    if (ntohl(out->magic) != MAGIC_NUMBER)      return -1;
    return 0;
}

/*
 * do_pull_mode()
 *
 * --get 플래그 진입점.
 * 성공 시 0, 실패 시 1 반환.
 */
static int do_pull_mode(void) {
    fprintf(stdout,
        "\n╔══════════════════════════════════════╗\n"
        "║   Mini CI  —  과제 파일 다운로드      ║\n"
        "╚══════════════════════════════════════╝\n"
        "  서버에서 현재 주차 배포 파일을 받습니다...\n\n");

    /* ── 1) 서버 연결 ── */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "  [오류] 소켓 생성 실패\n");
        return 1;
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &srv.sin_addr) != 1) {
        fprintf(stderr, "  [오류] 서버 IP 주소가 올바르지 않습니다.\n");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        fprintf(stderr, "  [오류] 서버에 연결할 수 없습니다: %s\n", strerror(errno));
        close(sock);
        return 1;
    }

    /* ── 2) GetFilesHeader 전송 ── */
    GetFilesHeader req;
    memset(&req, 0, sizeof(req));
    req.magic       = htonl(MAGIC_NUMBER);
    req.version     = PROTO_VERSION;
    req.pkt_type    = PKT_GET_FILES;
    req.header_size = htons((uint16_t)sizeof(GetFilesHeader));
    strncpy(req.student_id, "anonymous", MAX_STUDENT_ID - 1);

    size_t sent = 0;
    const uint8_t *p = (const uint8_t *)&req;
    while (sent < sizeof(req)) {
        ssize_t n = send(sock, p + sent, sizeof(req) - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            fprintf(stderr, "  [오류] 요청 전송 실패: %s\n", strerror(errno));
            close(sock); return 1;
        }
        sent += (size_t)n;
    }

    /* ── 3) 수신 루프 ── */
    PullState state        = PULL_STATE_WAIT_META;
    int       out_fd       = -1;             /* 현재 저장 중인 파일 fd  */
    char      out_path[512] = {0};           /* 저장 경로               */
    uint8_t   out_exec     = 0;             /* chmod +x 필요 여부      */
    uint32_t  crc_accum    = 0xFFFFFFFF;    /* 수신 CRC 누적            */
    int       files_done   = 0;

    while (1) {
        /* 공통 헤더 수신 */
        CommonHeader ch;
        if (recv_common_header(sock, &ch) < 0) {
            fprintf(stderr, "\n  [오류] 수신 중 연결 끊김\n");
            if (out_fd >= 0) close(out_fd);
            close(sock); return 1;
        }

        uint16_t hdr_size = ntohs(ch.header_size);

        switch (ch.pkt_type) {

        /* ── RES_FILE_META: 새 파일 수신 시작 ── */
        case RES_FILE_META: {
            if (hdr_size != sizeof(FileMetaHeader)) goto recv_error;
            
            /* 공통 헤더 이후의 나머지 필드 수신 */
            FileMetaHeader meta;
            memset(&meta, 0, sizeof(meta));
            uint16_t rest = hdr_size - sizeof(CommonHeader);
            if (_recv_all(sock,
                          (uint8_t *)&meta + sizeof(CommonHeader),
                          rest) < 0) goto recv_error;

            meta.filename[MAX_FILENAME - 1] = '\0';
            
            /* 경로 조작 방어 */
            if (strstr(meta.filename, "..") || strchr(meta.filename, '/') || 
                strchr(meta.filename, '\\') || meta.filename[0] == '\0') {
                goto recv_error;
            }

            uint32_t fsize = ntohl(meta.file_size);
            out_exec = meta.is_executable;

            /* 현재 디렉토리에 파일명으로 저장 */
            int nw = snprintf(out_path, sizeof(out_path), "./%s", meta.filename);
            if (nw < 0 || (size_t)nw >= sizeof(out_path)) goto recv_error;
            
            out_fd = open(out_path,
                          O_WRONLY | O_CREAT | O_TRUNC,
                          0644);
            if (out_fd < 0) {
                fprintf(stderr,
                        "  [오류] 파일 생성 실패: %s — %s\n",
                        out_path, strerror(errno));
                goto recv_error;
            }

            crc_accum = 0xFFFFFFFF;   /* CRC 리셋 */
            state = PULL_STATE_WAIT_CHUNK;

            fprintf(stdout, "  ▼ %-30s  %u bytes",
                    meta.filename, fsize);
            fflush(stdout);
            break;
        }

        /* ── RES_FILE_CHUNK: 청크 데이터 수신 → 파일에 write ── */
        case RES_FILE_CHUNK: {
            if (state != PULL_STATE_WAIT_CHUNK || out_fd < 0)
                goto recv_error;

            /* FileChunkHeader 나머지 필드 수신 */
            if (hdr_size != sizeof(FileChunkHeader)) goto recv_error;
            FileChunkHeader chk;
            uint16_t rest = hdr_size - sizeof(CommonHeader);
            if (_recv_all(sock,
                          (uint8_t *)&chk + sizeof(CommonHeader),
                          rest) < 0) goto recv_error;

            uint32_t csz = ntohl(chk.chunk_size);
            if (csz == 0 || csz > FILE_CHUNK_SIZE * 2) goto recv_error;

            uint8_t *buf = malloc(csz);
            if (!buf) goto recv_error;

            if (_recv_all(sock, buf, csz) < 0) {
                free(buf); goto recv_error;
            }

            /* CRC 누적 */
            for (uint32_t i = 0; i < csz; i++) {
                crc_accum ^= buf[i];
                for (int j = 0; j < 8; j++)
                    crc_accum = (crc_accum >> 1)
                                ^ (0xEDB88320u & -(crc_accum & 1u));
            }

            /* 파일에 기록 */
            size_t written = 0;
            while (written < csz) {
                ssize_t w = write(out_fd, buf + written, csz - written);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) {
                    free(buf); goto recv_error;
                }
                written += w;
            }
            free(buf);

            /* 진행 표시 */
            putchar('.'); fflush(stdout);
            break;
        }

        /* ── RES_FILE_END: 파일 1개 완료 — CRC 검증 + chmod ── */
        case RES_FILE_END: {
            if (out_fd < 0) goto recv_error;

            /* FileEndHeader 나머지 필드 수신 */
            if (hdr_size != sizeof(FileEndHeader)) goto recv_error;
            FileEndHeader end;
            uint16_t rest = hdr_size - sizeof(CommonHeader);
            if (_recv_all(sock,
                          (uint8_t *)&end + sizeof(CommonHeader),
                          rest) < 0) goto recv_error;

            close(out_fd);
            out_fd = -1;

            uint32_t expected = ntohl(end.checksum);
            uint32_t actual   = ~crc_accum;

            if (expected != actual) {
                fprintf(stdout, "  [CRC 불일치!] 파일이 손상되었습니다.\n");
                /* 손상된 파일 삭제 */
                unlink(out_path);
            } else {
                /* chmod +x (서버가 실행 파일로 표시한 경우) */
                if (out_exec) {
                    if (chmod(out_path, 0755) < 0) {
                        fprintf(stderr, "  [경고] 실행 권한 설정 실패: %s\n", strerror(errno));
                    }
                    fprintf(stdout, "  ✓  (+x)\n");
                } else {
                    fprintf(stdout, "  ✓\n");
                }
                files_done++;
            }

            state = PULL_STATE_WAIT_META;
            break;
        }

        /* ── RES_FILE_DONE: 전체 완료 ── */
        case RES_FILE_DONE: {
            if (hdr_size < sizeof(CommonHeader)) goto recv_error;
            /* 나머지 필드 수신 (checksum 등 — 여기선 무시) */
            uint16_t rest = hdr_size - sizeof(CommonHeader);
            uint8_t  discard[64];
            if (rest > 0 && rest <= sizeof(discard))
                _recv_all(sock, discard, rest);

            fprintf(stdout,
                "\n  ══════════════════════════════════\n"
                "  다운로드 완료: %d개 파일\n"
                "  현재 디렉토리에 저장되었습니다.\n"
                "  ══════════════════════════════════\n\n",
                files_done);
            close(sock);
            return 0;
        }

        /* ── RES_FILE_ERROR: 서버 에러 메시지 출력 ── */
        case RES_FILE_ERROR: {
            if (hdr_size != sizeof(FileErrorHeader)) goto recv_error;
            FileErrorHeader err;
            uint16_t rest = hdr_size - sizeof(CommonHeader);
            if (_recv_all(sock,
                          (uint8_t *)&err + sizeof(CommonHeader),
                          rest) == 0) {
                err.message[sizeof(err.message) - 1] = '\0';
                fprintf(stderr, "\n  [서버 오류] %s\n\n", err.message);
            } else {
                fprintf(stderr, "\n  [서버 오류] 메시지 수신 실패\n\n");
            }

            if (out_fd >= 0) { close(out_fd); unlink(out_path); }
            close(sock);
            return 1;
        }

        default:
            fprintf(stderr, "\n  [오류] 알 수 없는 pkt_type: %u\n",
                    ch.pkt_type);
            goto recv_error;
        }

        continue;   /* 다음 패킷으로 */

    recv_error:
        fprintf(stderr, "\n  [오류] 수신 처리 중 오류 발생\n");
        if (out_fd >= 0) { close(out_fd); unlink(out_path); }
        close(sock);
        return 1;
    }

    /* 정상 도달 불가 */
    close(sock);
    return 1;
}

int main(int argc, char *argv[]) {
    /* ── [신규] --get 플래그 감지 ─────────────────────────────
     * ./client_bin --get  →  다운로드 모드, 이후 로직으로 내려가지 않음
     * ./client_bin <파일> →  기존 제출 모드, 아무것도 바뀌지 않음
     * ─────────────────────────────────────────────────────────── */
    if (argc >= 2 && strcmp(argv[1], "--get") == 0) {
        int ret = do_pull_mode();
        return ret;
    }

    /* ── 기존 제출 모드 코드 (1바이트도 수정 안 함) ─────────── */
    if (argc < 2) {
        fprintf(stderr, "사용법: %s <감시할_소스파일.c>\n", argv[0]);
        fprintf(stderr, "       %s --get             (과제 파일 다운로드)\n",
                argv[0]);
        return 1;
    }

    /* [기능 1] 학번 한 번만 입력 */
    char student_id[MAX_STUDENT_ID] = {0};
    prompt_student_id(student_id, sizeof(student_id));

    int num_files = argc - 1;
    if (num_files > MAX_FILES) num_files = MAX_FILES;

    ResultHeader results[MAX_FILES];
    memset(results, 0, sizeof(results));

    /* 배치 전송 */
    for (int i = 0; i < num_files; i++) {
        send_source(student_id, argv[i + 1], &results[i]);
    }

    /* ncurses UI */
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN,   COLOR_BLACK);   /* AC        */
        init_pair(2, COLOR_RED,     COLOR_BLACK);   /* WA/CE/RE  */
        init_pair(3, COLOR_YELLOW,  COLOR_BLACK);   /* TLE       */
        init_pair(4, COLOR_MAGENTA, COLOR_BLACK);   /* Security  */
        init_pair(5, COLOR_WHITE,   COLOR_BLUE);    /* Status    */
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    WINDOW *list_win   = newwin(max_y / 2 - 1, max_x, 1,          0);
    WINDOW *detail_win = newwin(max_y / 2,     max_x, max_y / 2,  0);

    keypad(list_win, TRUE);

    int selected = 0, ch = 0;

    while (ch != 'q') {
        wclear(list_win);
        wclear(detail_win);
        box(list_win,   0, 0);
        box(detail_win, 0, 0);

        mvwprintw(list_win, 1, 2,
                  "[ CI Server Results ] UP/DOWN: select  ENTER: open editor  q: quit");

        for (int i = 0; i < num_files; i++) {
            if (i == selected) wattron(list_win, A_REVERSE);

            static const char *gnames[] = {"AC","WA","TLE","RE","CE"};
            uint8_t r  = results[i].result < 5 ? results[i].result : 4;
            int pair   = (r == GRADE_AC) ? 1 :
                         (r == GRADE_TLE) ? 3 : 2;

            const char *bname = strrchr(argv[i + 1], '/');
            bname = bname ? bname + 1 : argv[i + 1];

            mvwprintw(list_win, 3 + i, 4, "%-32s : ", bname);
            if (has_colors()) wattron(list_win, COLOR_PAIR(pair) | A_BOLD);
            wprintw(list_win, "%-4s", gnames[r]);
            if (has_colors()) wattroff(list_win, COLOR_PAIR(pair) | A_BOLD);
            wprintw(list_win, "  %ums", ntohl(results[i].exec_time_ms));

            if (i == selected) wattroff(list_win, A_REVERSE);
        }

        /* 상세 패널 */
        mvwprintw(detail_win, 1, 2,
                  "[ Detail: %s ]",
                  strrchr(argv[selected + 1], '/') ?
                  strrchr(argv[selected + 1], '/') + 1 : argv[selected + 1]);

        int line = 3;
        char tmp[128];
        strncpy(tmp, results[selected].message, 127);
        tmp[127] = '\0';
        char *tok = strtok(tmp, "\n");
        while (tok && line < max_y / 2 - 1) {
            mvwprintw(detail_win, line++, 4, "%s", tok);
            tok = strtok(NULL, "\n");
        }

        wrefresh(list_win);
        wrefresh(detail_win);
        draw_status_bar();

        ch = wgetch(list_win);
        if (ch == KEY_UP   && selected > 0)            selected--;
        else if (ch == KEY_DOWN && selected < num_files - 1) selected++;
        else if (ch == '\n') {
            /* 에디터 점프 후 자동 재채점 */
            def_prog_mode();
            endwin();
            jump_to_editor(argv[selected + 1], 1);
            send_source(student_id, argv[selected + 1], &results[selected]);
            reset_prog_mode();
            clear();
        }
    }

    endwin();
    return 0;
}
