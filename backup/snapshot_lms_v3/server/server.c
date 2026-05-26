#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <errno.h>
#include <locale.h>
#include <ncurses.h>
#include <ftw.h>       // [보안 수정 5] nftw를 이용한 안전한 디렉토리 삭제

#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <stdatomic.h>

#include <seccomp.h>
#include <sys/capability.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "monitor.skel.h"
#include "../include/protocol.h"
#include "../ipc_protocol.h"
#include "../server_ctx.h"

#define NUM_THREADS 4
#define MAX_QUEUE 100

#define CTL_SOCK_PATH    "/tmp/minici.sock"
#define MAX_EPOLL_EVENTS 16

static atomic_int g_paused = 0;
atomic_int timeout_seconds = 2;

// ─────────────────────────────────────────────
//  Ncurses 색상 쌍 정의
// ─────────────────────────────────────────────
#define COLOR_PAIR_IDLE      1   // 초록색: 스레드 대기 중
#define COLOR_PAIR_RUNNING   2   // 노란색: 스레드 실행 중
#define COLOR_PAIR_ALERT     3   // 빨간색 굵음: 보안 경고
#define COLOR_PAIR_INFO      4   // 시안색: 일반 정보 로그
#define COLOR_PAIR_HEADER    5   // 파란색 배경: 헤더 바
#define COLOR_PAIR_SUCCESS   6   // 밝은 초록: AC 결과
#define COLOR_PAIR_FAIL      7   // 마젠타: WA / 오류 결과
#define COLOR_PAIR_DIM       8   // 어두운 흰색: 기타

WINDOW *top_win    = NULL;
WINDOW *bottom_win = NULL;
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

static FILE *g_log_fp = NULL;
static int   g_ncurses_ok = 0;

void ui_logfile_init() {
    g_log_fp = fopen("/tmp/ci_server.log", "a");
    if (!g_log_fp) {
        fprintf(stderr, "[WARN] Cannot open /tmp/ci_server.log\n");
    }
}

void ui_ncurses_ready() {
    g_ncurses_ok = 1;
}

void ui_diagnose() {
    if (g_log_fp) {
        fprintf(g_log_fp, "[DIAGNOSE] Ncurses UI is successfully initialized.\n");
        fflush(g_log_fp);
    }
}

// Thread Pool 상태
int  thread_status[NUM_THREADS];
char thread_task[NUM_THREADS][128];

int job_queue[MAX_QUEUE];
int q_head = 0, q_tail = 0, q_count = 0;
pthread_mutex_t q_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  q_cond  = PTHREAD_COND_INITIALIZER;

#define EVENT_SYSCALL_BLOCKED   1
#define EVENT_PROCESS_ENTER     2
#define EVENT_PROCESS_EXIT      3

// [보안 수정 1] BPF 키와 동기화: 복합 pid_key 구조체
struct pid_key {
    __u32 pid;
    __u32 cgroup_tag;
};

struct event {
    __u64  timestamp_ns;
    __u32  pid;
    __u32  tgid;
    __u32  event_type;
    __u32  syscall_nr;
    char   comm[16];
};

// ─────────────────────────────────────────────
//  UI 헬퍼 함수
// ─────────────────────────────────────────────

// 색상 쌍과 속성을 지정해서 로그를 bottom_win에 출력
void ui_log_colored(int color_pair, attr_t attr, const char *fmt, ...) {
    pthread_mutex_lock(&ui_mutex);
    if (g_ncurses_ok && bottom_win) {
        wattron(bottom_win, COLOR_PAIR(color_pair) | attr);
        va_list args;
        va_start(args, fmt);
        vw_printw(bottom_win, fmt, args);
        va_end(args);
        wattroff(bottom_win, COLOR_PAIR(color_pair) | attr);
        wrefresh(bottom_win);
    } else {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fflush(stderr);
    }
    if (g_log_fp) {
        va_list args;
        va_start(args, fmt);
        vfprintf(g_log_fp, fmt, args);
        va_end(args);
        fflush(g_log_fp);
    }
    pthread_mutex_unlock(&ui_mutex);
}

// 편의 매크로
#define ui_log_info(fmt, ...)    ui_log_colored(COLOR_PAIR_INFO,    A_NORMAL, fmt, ##__VA_ARGS__)
#define ui_log_alert(fmt, ...)   ui_log_colored(COLOR_PAIR_ALERT,   A_BOLD,   fmt, ##__VA_ARGS__)
#define ui_log_success(fmt, ...) ui_log_colored(COLOR_PAIR_SUCCESS, A_NORMAL, fmt, ##__VA_ARGS__)
#define ui_log_fail(fmt, ...)    ui_log_colored(COLOR_PAIR_FAIL,    A_NORMAL, fmt, ##__VA_ARGS__)
#define ui_log_dim(fmt, ...)     ui_log_colored(COLOR_PAIR_DIM,     A_DIM,    fmt, ##__VA_ARGS__)

// 기존 코드와의 호환성을 위한 일반 로그 (시안색)
void ui_log(const char *fmt, ...) {
    pthread_mutex_lock(&ui_mutex);
    if (g_ncurses_ok && bottom_win) {
        wattron(bottom_win, COLOR_PAIR(COLOR_PAIR_INFO));
        va_list args;
        va_start(args, fmt);
        vw_printw(bottom_win, fmt, args);
        va_end(args);
        wattroff(bottom_win, COLOR_PAIR(COLOR_PAIR_INFO));
        wrefresh(bottom_win);
    } else {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fflush(stderr);
    }
    if (g_log_fp) {
        va_list args;
        va_start(args, fmt);
        vfprintf(g_log_fp, fmt, args);
        va_end(args);
        fflush(g_log_fp);
    }
    pthread_mutex_unlock(&ui_mutex);
}

void update_top_win() {
    pthread_mutex_lock(&ui_mutex);
    if (!g_ncurses_ok || !top_win) { pthread_mutex_unlock(&ui_mutex); return; }

    werase(top_win);

    // 헤더 배경 바
    wattron(top_win, COLOR_PAIR(COLOR_PAIR_HEADER) | A_BOLD);
    mvwhline(top_win, 0, 0, ' ', getmaxx(top_win));
    mvwprintw(top_win, 0, 2, " ██ Mini CI Server — Thread Pool Monitor ██ ");
    wattroff(top_win, COLOR_PAIR(COLOR_PAIR_HEADER) | A_BOLD);

    for (int i = 0; i < NUM_THREADS; i++) {
        mvwprintw(top_win, i + 1, 2, "Thread %d: ", i);

        if (thread_status[i] == 0) {
            wattron(top_win, COLOR_PAIR(COLOR_PAIR_IDLE) | A_BOLD);
            wprintw(top_win, "[ IDLE    ]");
            wattroff(top_win, COLOR_PAIR(COLOR_PAIR_IDLE) | A_BOLD);
        } else {
            wattron(top_win, COLOR_PAIR(COLOR_PAIR_RUNNING) | A_BOLD);
            wprintw(top_win, "[ RUNNING ]");
            wattroff(top_win, COLOR_PAIR(COLOR_PAIR_RUNNING) | A_BOLD);
            wattron(top_win, COLOR_PAIR(COLOR_PAIR_DIM));
            wprintw(top_win, " %s", thread_task[i]);
            wattroff(top_win, COLOR_PAIR(COLOR_PAIR_DIM));
        }
    }

    // 하단 구분선에 큐 상태 표시
    int bottom_row = NUM_THREADS + 1;
    wattron(top_win, COLOR_PAIR(COLOR_PAIR_DIM) | A_DIM);
    mvwhline(top_win, bottom_row, 0, ACS_HLINE, getmaxx(top_win));
    mvwprintw(top_win, bottom_row, 2, " Queue: %d / %d ", q_count, MAX_QUEUE);
    wattroff(top_win, COLOR_PAIR(COLOR_PAIR_DIM) | A_DIM);

    wrefresh(top_win);
    pthread_mutex_unlock(&ui_mutex);
}

// ─────────────────────────────────────────────
//  시그널 핸들러
// ─────────────────────────────────────────────
void sigusr1_handler(int signum) { (void)signum; atomic_store(&timeout_seconds, 3); }
void sigint_handler(int signum)  { (void)signum; endwin(); exit(0); }

// ─────────────────────────────────────────────
//  Job Queue
// ─────────────────────────────────────────────
void enqueue_job(int sock) {
    pthread_mutex_lock(&q_mutex);
    job_queue[q_tail] = sock;
    q_tail = (q_tail + 1) % MAX_QUEUE;
    q_count++;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_mutex);
}

int dequeue_job() {
    pthread_mutex_lock(&q_mutex);
    while (q_count == 0) pthread_cond_wait(&q_cond, &q_mutex);
    int sock = job_queue[q_head];
    q_head = (q_head + 1) % MAX_QUEUE;
    q_count--;
    pthread_mutex_unlock(&q_mutex);
    return sock;
}

// ─────────────────────────────────────────────
//  [보안 수정 5] 안전한 재귀 삭제 (symlink 공격 방지)
// ─────────────────────────────────────────────
static int safe_remove_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_DP || typeflag == FTW_D)
        return rmdir(path);
    return unlink(path);
}

static void safe_rmdir(const char *path) {
    // FTW_DEPTH: 하위 항목부터 삭제, FTW_PHYS: symlink를 따라가지 않음
    nftw(path, safe_remove_cb, 16, FTW_DEPTH | FTW_PHYS);
}

// ─────────────────────────────────────────────
//  출력 비교
// ─────────────────────────────────────────────
#include <ctype.h>
static int compare_output(const char *actual_file, const char *expected_file) {
    FILE *f_act = fopen(actual_file, "r");
    FILE *f_exp = fopen(expected_file, "r");
    if (!f_act || !f_exp) {
        if (f_act) fclose(f_act);
        if (f_exp) fclose(f_exp);
        return -1;
    }
    int c_act, c_exp;
    while (1) {
        do { c_act = fgetc(f_act); } while (isspace(c_act));
        do { c_exp = fgetc(f_exp); } while (isspace(c_exp));
        if (c_act == EOF && c_exp == EOF) { fclose(f_act); fclose(f_exp); return 1; }
        if (c_act != c_exp)               { fclose(f_act); fclose(f_exp); return 0; }
    }
}

// ─────────────────────────────────────────────
//  eBPF 이벤트 콜백
// ─────────────────────────────────────────────
static int handle_event(void *ctx, void *data, size_t data_sz) {
    long thread_id = (long)ctx;
    if (data_sz < sizeof(struct event)) return 0;
    struct event *e = (struct event *)data;

    if (e->event_type == EVENT_SYSCALL_BLOCKED) {
        const char *sys_name = "UNKNOWN";
        if      (e->syscall_nr == 59)  sys_name = "execve";
        else if (e->syscall_nr == 257) sys_name = "openat";
        else if (e->syscall_nr == 41)  sys_name = "socket";
        else if (e->syscall_nr == 42)  sys_name = "connect";

        // [UI] 보안 경고는 빨간색 + 굵음으로 강조
        ui_log_alert("  🚨 [Thread %ld] eBPF THREAT DETECTED | PID: %u | Syscall: %u (%s) → BLOCKED & KILLED!\n",
                     thread_id, e->pid, e->syscall_nr, sys_name);
    } else if (e->event_type == EVENT_PROCESS_ENTER) {
        ui_log_dim("  ↳ [Thread %ld] PID %u spawned (tracked)\n", thread_id, e->pid);
    } else if (e->event_type == EVENT_PROCESS_EXIT) {
        ui_log_dim("  ↳ [Thread %ld] PID %u exited (untracked)\n", thread_id, e->pid);
    }
    return 0;
}

// ─────────────────────────────────────────────
//  클라이언트 처리
// ─────────────────────────────────────────────

static void build_and_send_result(int          cli_fd,
                                  SubmitHeader hdr,
                                  GradeResult  grade,
                                  uint32_t     exec_ms,
                                  uint32_t     mem_kb) {
    ResultHeader resp;
    memset(&resp, 0, sizeof(resp));

    resp.magic        = htonl(MAGIC_NUMBER);
    resp.version      = PROTO_VERSION;
    resp.pkt_type     = RES_RESULT;
    resp.header_size  = htons((uint16_t)sizeof(ResultHeader));
    resp.result       = (uint8_t)grade;
    resp.exec_time_ms = htonl(exec_ms);
    resp.memory_kb    = htonl(mem_kb);

    strncpy(resp.student_id, hdr.student_id, MAX_STUDENT_ID);
    strncpy(resp.filename,   hdr.filename,   MAX_FILENAME);

    pthread_rwlock_rdlock(&g_ctx.hw_lock);
    strncpy(resp.week_name, g_ctx.current_hw, MAX_WEEK_NAME);
    pthread_rwlock_unlock(&g_ctx.hw_lock);

    pthread_mutex_lock(&g_ctx.tracker_lock);
    resp.submitted_count = htons(g_ctx.submitted_count);
    resp.total_students  = htons(g_ctx.total_students);
    pthread_mutex_unlock(&g_ctx.tracker_lock);

    if (send(cli_fd, &resp, sizeof(resp), MSG_NOSIGNAL) < 0)
        perror("[worker] send ResultHeader");

    ctx_record_submission(&g_ctx, hdr.student_id, grade);
    ctx_write_csv(&g_ctx, hdr.student_id, grade, exec_ms, mem_kb);

    static const char *gnames[] = {"AC","WA","TLE","RE","CE"};
    const char *gs = (grade < 5) ? gnames[grade] : "UNK";
    fprintf(stderr,
            "[grade] [%-8s] %-32s %-4s  %4ums  %4uKB\n",
            hdr.student_id, hdr.filename, gs, exec_ms, mem_kb);
}

#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ── 내부 헬퍼: CRC32 (protocol.h 에 이미 있으면 중복 정의 제거) ── */
__attribute__((unused))
static uint32_t _pull_crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

/* ── 내부 헬퍼: 고정 크기 헤더 전송 (부분 전송 재시도 포함) ── */
static int _send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* ── 내부 헬퍼: 파일 한 개 전송 ──────────────────────────────────
 * file_path : 서버 로컬 절대(또는 상대) 경로
 * filename  : 클라이언트에게 알려줄 베이스 파일명
 * index     : 1-based 파일 번호
 * total     : 전체 배포 파일 수
 * 반환값    : 0 성공, -1 실패
 * ─────────────────────────────────────────────────────────────── */
static int send_one_file(int         cli_fd,
                         const char *file_path,
                         const char *filename,
                         uint8_t     index,
                         uint8_t     total) {
    /* 1) 파일 열기 */
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[pull] open 실패: %s — %s\n",
                file_path, strerror(errno));
        return -1;
    }

    struct stat st;
    fstat(fd, &st);
    uint32_t fsize = (uint32_t)st.st_size;

    /* 실행 권한 여부 판단 (owner execute bit) */
    uint8_t is_exec = (st.st_mode & S_IXUSR) ? 1 : 0;

    /* 2) FileMetaHeader 전송 */
    FileMetaHeader meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic       = htonl(MAGIC_NUMBER);
    meta.version     = PROTO_VERSION;
    meta.pkt_type    = RES_FILE_META;
    meta.header_size = htons((uint16_t)sizeof(FileMetaHeader));
    meta.file_size   = htonl(fsize);
    meta.is_executable = is_exec;
    meta.file_index  = index;
    meta.total_files = total;
    strncpy(meta.filename, filename, MAX_FILENAME - 1);

    if (_send_all(cli_fd, &meta, sizeof(meta)) < 0) {
        close(fd); return -1;
    }

    /* 3) 파일 내용을 청크 단위로 전송 + CRC32 누적 계산 */
    uint8_t *chunk_buf = malloc(FILE_CHUNK_SIZE);
    if (!chunk_buf) { close(fd); return -1; }

    uint32_t crc      = 0xFFFFFFFF;
    uint32_t offset   = 0;
    ssize_t  nread;

    while ((nread = read(fd, chunk_buf, FILE_CHUNK_SIZE)) > 0) {
        /* CRC 누적 */
        for (ssize_t i = 0; i < nread; i++) {
            crc ^= chunk_buf[i];
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }

        /* FileChunkHeader 전송 */
        FileChunkHeader chk;
        memset(&chk, 0, sizeof(chk));
        chk.magic       = htonl(MAGIC_NUMBER);
        chk.version     = PROTO_VERSION;
        chk.pkt_type    = RES_FILE_CHUNK;
        chk.header_size = htons((uint16_t)sizeof(FileChunkHeader));
        chk.chunk_size  = htonl((uint32_t)nread);
        chk.offset      = htonl(offset);

        if (_send_all(cli_fd, &chk, sizeof(chk)) < 0 ||
            _send_all(cli_fd, chunk_buf, (size_t)nread) < 0) {
            free(chunk_buf); close(fd); return -1;
        }
        offset += (uint32_t)nread;
    }
    free(chunk_buf);
    close(fd);

    /* 4) FileEndHeader (RES_FILE_END) 전송 */
    FileEndHeader end;
    memset(&end, 0, sizeof(end));
    end.magic       = htonl(MAGIC_NUMBER);
    end.version     = PROTO_VERSION;
    end.pkt_type    = RES_FILE_END;
    end.header_size = htons((uint16_t)sizeof(FileEndHeader));
    end.checksum    = htonl(~crc);     /* CRC32 최종값 */

    return _send_all(cli_fd, &end, sizeof(end));
}

/*
 * handle_file_pull()
 *
 * PKT_GET_FILES 를 받았을 때 호출되는 메인 배포 함수.
 * g_ctx.current_hw 로부터 배포 폴더를 결정하고,
 * 그 안의 파일을 전부 클라이언트에게 전송한다.
 *
 * 배포 폴더 경로: DIST_ROOT "/" current_hw
 * 예)  "dist/week_3"
 */
void handle_file_pull(int cli_fd, const char *student_id) {

    /* ── 1) 현재 주차로 배포 폴더 경로 결정 ── */
    char dist_dir[512];
    pthread_rwlock_rdlock(&g_ctx.hw_lock);
    snprintf(dist_dir, sizeof(dist_dir),
             "%s/%s", DIST_ROOT, g_ctx.current_hw);
    pthread_rwlock_unlock(&g_ctx.hw_lock);

    fprintf(stderr, "[pull] 요청: student=%s  dist_dir=%s\n",
            student_id[0] ? student_id : "anonymous", dist_dir);

    /* ── 1.5) 배포 중지 상태 확인 ── */
    int is_paused = 0;
    pthread_mutex_lock(&g_ctx.state_lock);
    is_paused = g_ctx.dist_paused;
    pthread_mutex_unlock(&g_ctx.state_lock);

    if (is_paused) {
        FileErrorHeader err;
        memset(&err, 0, sizeof(err));
        err.magic       = htonl(MAGIC_NUMBER);
        err.version     = PROTO_VERSION;
        err.pkt_type    = RES_FILE_ERROR;
        err.header_size = htons((uint16_t)sizeof(FileErrorHeader));
        snprintf(err.message, sizeof(err.message),
                 "현재 교수님이 파일 배포를 일시 중지했습니다.");
        _send_all(cli_fd, &err, sizeof(err));
        fprintf(stderr, "[pull] 거절: 배포 중지 상태\n");
        return;
    }

    /* ── 2) 배포 폴더 스캔 → 파일 목록 수집 ── */
    DIR *dir = opendir(dist_dir);
    if (!dir) {
        /* 폴더가 없거나 열 수 없음 → 에러 패킷 전송 */
        FileErrorHeader err;
        memset(&err, 0, sizeof(err));
        err.magic       = htonl(MAGIC_NUMBER);
        err.version     = PROTO_VERSION;
        err.pkt_type    = RES_FILE_ERROR;
        err.header_size = htons((uint16_t)sizeof(FileErrorHeader));
        snprintf(err.message, sizeof(err.message),
                 "배포 폴더 없음: %.60s  (교수님께 문의하세요)", dist_dir);
        _send_all(cli_fd, &err, sizeof(err));

        fprintf(stderr, "[pull] 에러: 폴더 없음 — %s\n", dist_dir);
        return;
    }

    /* 일반 파일만 추려서 목록 배열에 저장 */
    char file_paths[MAX_DIST_FILES][512];
    char file_names[MAX_DIST_FILES][MAX_FILENAME];
    int  file_count = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && file_count < MAX_DIST_FILES) {
        if (ent->d_name[0] == '.') continue;  /* 숨김 파일 / . / .. 제외 */

        char full[512];
        snprintf(full, sizeof(full), "%.255s/%.255s", dist_dir, ent->d_name);

        struct stat st;
        if (stat(full, &st) < 0) continue;
        if (!S_ISREG(st.st_mode)) continue;   /* 디렉토리 등 제외         */

        strncpy(file_paths[file_count], full,      511);
        strncpy(file_names[file_count], ent->d_name, MAX_FILENAME - 1);
        file_paths[file_count][511]          = '\0';
        file_names[file_count][MAX_FILENAME-1] = '\0';
        file_count++;
    }
    closedir(dir);

    /* ── 3) 파일이 하나도 없으면 에러 ── */
    if (file_count == 0) {
        FileErrorHeader err;
        memset(&err, 0, sizeof(err));
        err.magic       = htonl(MAGIC_NUMBER);
        err.version     = PROTO_VERSION;
        err.pkt_type    = RES_FILE_ERROR;
        err.header_size = htons((uint16_t)sizeof(FileErrorHeader));
        snprintf(err.message, sizeof(err.message),
                 "배포 파일 없음: %.50s  (교수님이 아직 업로드하지 않았습니다)",
                 dist_dir);
        _send_all(cli_fd, &err, sizeof(err));

        fprintf(stderr, "[pull] 에러: 파일 없음 — %s\n", dist_dir);
        return;
    }

    fprintf(stderr, "[pull] %d개 파일 전송 시작\n", file_count);

    /* ── 4) 파일별 전송 ── */
    for (int i = 0; i < file_count; i++) {
        int ret = send_one_file(cli_fd,
                                file_paths[i],
                                file_names[i],
                                (uint8_t)(i + 1),
                                (uint8_t)file_count);
        if (ret < 0) {
            fprintf(stderr, "[pull] 전송 실패: %s\n", file_names[i]);
            /* 클라이언트 연결이 끊긴 것으로 간주하고 루프 탈출 */
            return;
        }
        fprintf(stderr, "[pull] 전송 완료: %s\n", file_names[i]);
    }

    /* ── 5) 전체 전송 완료 신호 (RES_FILE_DONE) ── */
    FileEndHeader done;
    memset(&done, 0, sizeof(done));
    done.magic       = htonl(MAGIC_NUMBER);
    done.version     = PROTO_VERSION;
    done.pkt_type    = RES_FILE_DONE;
    done.header_size = htons((uint16_t)sizeof(FileEndHeader));
    done.total_files = (uint8_t)file_count;
    _send_all(cli_fd, &done, sizeof(done));

    fprintf(stderr, "[pull] 완료: student=%s  %d개 파일 전송됨\n",
            student_id[0] ? student_id : "anonymous", file_count);
}

void handle_client(int client_sock, int thread_id) {
    char *work_dir = NULL;
    char template[] = "/tmp/ci_workspace_XXXXXX";
    ui_log_info("[Trace] Thread %d: handle_client started\n", thread_id);
    SubmitHeader fheader;
    memset(&fheader, 0, sizeof(fheader));

    // 1) 8바이트 (CommonHeader 크기) 먼저 읽기
    if (recv(client_sock, &fheader, 8, MSG_WAITALL) <= 0) {
        close(client_sock); return;
    }

    /*
     * [분기 추가] PKT_GET_FILES 요청이면 배포 로직으로 이동.
     * PKT_SUBMIT 이면 기존 채점 경로를 그대로 탄다.
     */
    if (fheader.pkt_type == PKT_GET_FILES) {
        size_t rest = sizeof(GetFilesHeader) - 8;
        if (recv(client_sock, (char *)&fheader + 8, rest, MSG_WAITALL) <= 0) {
            close(client_sock); return;
        }
        /* ── 배포 로직으로 분기, 채점 코드로는 절대 내려가지 않음 ── */
        handle_file_pull(client_sock, fheader.student_id);
        goto client_cleanup;   /* 채점 루틴 완전히 건너뜀           */
    }

    // 2) 일반 제출이면 SubmitHeader의 나머지 부분 수신
    size_t rest = sizeof(SubmitHeader) - 8;
    if (recv(client_sock, (char *)&fheader + 8, rest, MSG_WAITALL) <= 0) {
        close(client_sock); return;
    }

    ui_log_info("[Trace] Thread %d: Header received (file: %s, size: %u)\n", thread_id, fheader.filename, fheader.file_size);

    work_dir = mkdtemp(template);
    if (!work_dir) { close(client_sock); return; }

    // 소스 파일 수신
    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/source.c", work_dir);
    int fd_src = open(src_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_src >= 0) {
        char buf[4096]; long remain = fheader.file_size;
        ui_log_info("[Trace] Thread %d: Receiving source code...\n", thread_id);
        while (remain > 0) {
            int to_read = remain > (long)sizeof(buf) ? (int)sizeof(buf) : remain;
            int n = recv(client_sock, buf, to_read, 0);
            if (n <= 0) break;
            if (write(fd_src, buf, n) < 0) {}
            remain -= n;
        }
        close(fd_src);
        ui_log_info("[Trace] Thread %d: Source code saved.\n", thread_id);
    }

    // 테스트 입력 수신 — 서버 측 케이스 사용 (ans_path는 g_ctx.hw_lock으로 관리)

    int pipefd[2];
    if (pipe(pipefd) < 0) {}

    GradeResult final_res = GRADE_AC;
    uint32_t final_exec = 0;
    uint32_t final_mem = 0;

    // ── 컴파일 단계 ──
    ui_log_info("[Trace] Thread %d: Forking gcc...\n", thread_id);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        if (chdir(work_dir) < 0) _exit(EXIT_FAILURE);
        char *args[] = {"gcc", "-O2", "-static", "-o", "test.out", "source.c",
                        "-fstack-protector-strong", "-D_FORTIFY_SOURCE=2", NULL};
        execvp(args[0], args);
        _exit(EXIT_FAILURE);
    }
    close(pipefd[1]);
    int status;
    ui_log_info("[Trace] Thread %d: Waiting for gcc (waitpid)...\n", thread_id);
    waitpid(pid, &status, 0);
    ui_log_info("[Trace] Thread %d: gcc finished. WIFEXITED=%d, WEXITSTATUS=%d\n", thread_id, WIFEXITED(status), WEXITSTATUS(status));

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        // 컴파일 실패
        final_res = GRADE_CE;
        ui_log_info("[Trace] Thread %d: Reading gcc error from pipe...\n", thread_id);
        char err_msg[256] = {0};
        ssize_t r = read(pipefd[0], err_msg, sizeof(err_msg) - 1);
        if (r >= 0) err_msg[r] = '\0';
        ui_log_fail("[Thread %d] Compile Error: %s\n", thread_id, fheader.filename);
        ui_log_info("[Trace] Thread %d: Sending response to client...\n", thread_id);
    } else {
        // ── 실행 단계 ──
        char in_path[512];
        snprintf(in_path, sizeof(in_path), "server/test_cases/case1_in.txt");
        if (access(in_path, F_OK) != 0) snprintf(in_path, sizeof(in_path), "server/input.txt");
        int in_fd = open(in_path, O_RDONLY);

        char out_path[512];
        snprintf(out_path, sizeof(out_path), "%s/output.txt", work_dir);
        int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        // ── eBPF 설정 ──
        struct monitor_bpf *skel = monitor_bpf__open();
        struct ring_buffer *rb   = NULL;
        int bpf_map_fd = -1;

        if (skel && monitor_bpf__load(skel) == 0) {
            if (monitor_bpf__attach(skel) == 0) {
                static const __u64 blocked_list[] = {
                    57, 58, 56, 435, 322, 41, 42, 43, 44, 45, 46, 47, 48,
                    49, 50, 54, 55, 2, 85, 82, 257, 316, 263, 87, 88, 266, 265,
                    86, 83, 258, 84, 76, 77, 90, 91, 92, 62, 234, 200, 160, 161,
                    125, 158, 101, 175, 176, 313, 304, 237, 238, 279, 272, 308,
                    298, 321, 250, 251, 252, 155, 161, 167, 168, 165, 166,
                    425, 426, 427, // [보안 강화] io_uring 차단
                    0
                };
                bpf_map_fd = bpf_map__fd(skel->maps.blocked_syscalls);
                __u8 val = 1;
                for (int i = 0; blocked_list[i] != 0; i++) {
                    __u64 nr = blocked_list[i];
                    bpf_map_update_elem(bpf_map_fd, &nr, &val, BPF_ANY);
                }
            } else {
                ui_log_fail("[Thread %d] BPF attach failed\n", thread_id);
            }
        }

        // [보안 수정 4] run_sync_pipe 패턴 유지하되,
        // 내부 프로세스의 호스트 PID를 부모에게 전달하는 파이프를 추가.
        // 이를 통해 실제 실행 프로세스(inner)의 PID를 tracked_pids에 정확히 등록.
        int run_sync_pipe[2];
        int pid_report_pipe[2]; // [신규] inner pid를 부모에게 전달
        if (pipe(run_sync_pipe) < 0) {}
        if (pipe(pid_report_pipe) < 0) {}

        pid_t run_pid = fork();
        if (run_pid == 0) {
            // ── 격리 컨테이너 래퍼 프로세스 ──
            close(run_sync_pipe[1]);
            close(pid_report_pipe[0]);

            if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC |
                        CLONE_NEWNET | CLONE_NEWUTS) < 0) _exit(1);

            pid_t inner = fork();
            if (inner < 0) _exit(1);

            if (inner == 0) {
                // ── 실제 코드 실행 프로세스 ──
                close(pid_report_pipe[1]); // 자식은 쓰기 끝 닫기

                // 부모 신호 대기 (eBPF 등록 완료 기다림)
                char dummy;
                if (read(run_sync_pipe[0], &dummy, 1) < 0) {}
                close(run_sync_pipe[0]);

                struct rlimit rl;
                rl.rlim_cur = atomic_load(&timeout_seconds); rl.rlim_max = atomic_load(&timeout_seconds);
                setrlimit(RLIMIT_CPU, &rl);
                rl.rlim_cur = rl.rlim_max = 64 * 1024 * 1024;
                setrlimit(RLIMIT_AS, &rl);
                rl.rlim_cur = rl.rlim_max = 1 * 1024 * 1024;
                setrlimit(RLIMIT_FSIZE, &rl);
                rl.rlim_cur = rl.rlim_max = 1;
                setrlimit(RLIMIT_NPROC, &rl);

                // 모든 capability 드롭
                for (int cap = 0; cap <= CAP_LAST_CAP; cap++)
                    prctl(PR_CAPBSET_DROP, cap, 0, 0, 0);
                cap_t empty = cap_init();
                cap_set_proc(empty);
                cap_free(empty);
                prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

                // Seccomp 화이트리스트
                scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ERRNO(1));
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(read), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(write), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(close), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readv), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(writev), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fstat), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(newfstatat), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(brk), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(munmap), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mremap), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(clock_gettime), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(gettimeofday), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(time), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(arch_prctl), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(set_tid_address), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(set_robust_list), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getpid), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(gettid), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(futex), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(prlimit64), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(uname), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getrandom), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigaction), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigprocmask), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigreturn), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(exit), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(exit_group), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(execve), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(pread64), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(access), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rseq), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(sysinfo), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(lseek), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readlink), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readlinkat), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(stat), 0);
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(lstat), 0);
                if (in_fd >= 0)  { dup2(in_fd,  STDIN_FILENO);  close(in_fd);  }
                if (out_fd >= 0) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }

                if (chdir(work_dir) < 0) _exit(1);

                seccomp_load(ctx);
                seccomp_release(ctx);
                char *exec_args[] = {"./test.out", NULL};
                execv(exec_args[0], exec_args);
                const char err_msg[] = "execv failed\n";
                if (write(STDERR_FILENO, err_msg, sizeof(err_msg) - 1) < 0) {
                    /* 무시 (어차피 즉시 종료됨) */
                }
                _exit(1);
            } else {
                // 래퍼 프로세스: inner의 호스트 PID를 부모에게 전달
                // [보안 수정 4] inner의 PID를 pid_report_pipe로 전달
                close(run_sync_pipe[0]); // 래퍼는 동기화 파이프가 필요 없음
                if (write(pid_report_pipe[1], &inner, sizeof(inner)) < 0) {}
                close(pid_report_pipe[1]);

                int st;
                waitpid(inner, &st, 0);
                if (WIFSIGNALED(st)) {
                    // 자식이 시그널로 죽었으면 래퍼도 동일한 시그널로 자결하여 부모에게 전달
                    kill(getpid(), WTERMSIG(st));
                    sleep(1);
                    _exit(128 + WTERMSIG(st));
                }
                _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
            }
        }

        // ── 부모 프로세스: inner PID 수신 후 BPF 맵 등록 ──
        close(run_sync_pipe[0]);
        close(pid_report_pipe[1]);

        if (in_fd >= 0)  close(in_fd);
        if (out_fd >= 0) close(out_fd);

        ui_log_info("[Trace] Thread %d: Parent waiting for inner_pid...\n", thread_id);
        if (skel) {
            // [보안 수정 4] inner PID를 파이프로 받아 정확히 tracked_pids에 등록
            pid_t inner_pid = -1;
            ssize_t n = read(pid_report_pipe[0], &inner_pid, sizeof(inner_pid));
            close(pid_report_pipe[0]);
            ui_log_info("[Trace] Thread %d: Parent read inner_pid = %d\n", thread_id, inner_pid);

            if (n == sizeof(inner_pid) && inner_pid > 0) {
                int tracked_fd = bpf_map__fd(skel->maps.tracked_pids);
                // [보안 수정 1] cgroup_tag 없이 등록하는 기존 방식 대신,
                // pid_key 구조체를 사용. 단, cgroup_tag는 0으로 초기화(호스트 컨텍스트).
                // 실제 프로덕션에서는 cgroupfs를 통해 cgroup_id를 읽어와야 함.
                struct pid_key k = { .pid = (__u32)inner_pid, .cgroup_tag = 0 };
                __u32 sentinel = 0;
                bpf_map_update_elem(tracked_fd, &k, &sentinel, BPF_ANY);
            }

            rb = ring_buffer__new(bpf_map__fd(skel->maps.events),
                                  handle_event, (void *)(long)thread_id, NULL);
        } else {
            close(pid_report_pipe[0]);
        }

        // 자식 실행 허가 신호 전송
        ui_log_info("[Trace] Thread %d: Parent sending 'A' sync signal...\n", thread_id);
        if (write(run_sync_pipe[1], "A", 1) < 0) {}
        close(run_sync_pipe[1]);

        struct rusage usage;
        ui_log_info("[Trace] Thread %d: Parent waiting for wrapper to exit (wait4)...\n", thread_id);
        wait4(run_pid, &status, 0, &usage);
        ui_log_info("[Trace] Thread %d: Parent wait4 finished.\n", thread_id);

        // [보안 수정 6] ring_buffer 드레인: ENODATA가 나올 때까지 반복
        if (rb) {
            int err;
            int drain_retries = 20; // 최대 2초 드레인
            do {
                err = ring_buffer__poll(rb, 100);
                drain_retries--;
            } while (err > 0 && drain_retries > 0);
            ring_buffer__free(rb);
        }
        if (skel) monitor_bpf__destroy(skel);

        long user_ms  = usage.ru_utime.tv_sec * 1000 + usage.ru_utime.tv_usec / 1000;
        long max_rss  = usage.ru_maxrss;
        final_exec = user_ms;
        final_mem  = max_rss;

        if (WIFSIGNALED(status) && (WTERMSIG(status) == SIGALRM || WTERMSIG(status) == SIGXCPU)) {
            final_res = GRADE_TLE;
            ui_log_fail("[Thread %d] TLE: %s\n", thread_id, fheader.filename);
        } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS) {
            final_res = GRADE_RE;
            ui_log_alert("[Thread %d] 🚨 SIGSYS Security Violation: %s\n", thread_id, fheader.filename);
        } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
            final_res = GRADE_RE;
            ui_log_alert("[Thread %d] 🚨 eBPF Kill: %s\n", thread_id, fheader.filename);
        } else if (WIFSIGNALED(status)) {
            final_res = GRADE_RE;
            ui_log_fail("[Thread %d] RE Signal %d: %s\n", thread_id, WTERMSIG(status), fheader.filename);
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            final_res = GRADE_RE;
            ui_log_fail("[Thread %d] RE Non-zero: %s\n", thread_id, fheader.filename);
        } else {
            char expected_path[512];
            pthread_rwlock_rdlock(&g_ctx.hw_lock);
            strncpy(expected_path, g_ctx.ans_path, sizeof(expected_path));
            pthread_rwlock_unlock(&g_ctx.hw_lock);

            int diff_res = compare_output(out_path, expected_path);
            if (diff_res == 1) {
                final_res = GRADE_AC;
                ui_log_success("[Thread %d] AC: %s | %ld ms | %ld KB\n", thread_id, fheader.filename, user_ms, max_rss);
            } else if (diff_res == 0) {
                final_res = GRADE_WA;
                ui_log_fail("[Thread %d] WA: %s | %ld ms | %ld KB\n", thread_id, fheader.filename, user_ms, max_rss);
            } else {
                final_res = GRADE_AC;
                ui_log_info("[Thread %d] OK (no ans): %s | %ld ms | %ld KB\n", thread_id, fheader.filename, user_ms, max_rss);
            }
        }
    }

    close(pipefd[0]);
    build_and_send_result(client_sock, fheader, final_res, final_exec, final_mem);

client_cleanup:
    close(client_sock);

    // [보안 수정 5] fork+rm 대신 nftw 기반 안전한 삭제
    if (work_dir) safe_rmdir(work_dir);
}

// ─────────────────────────────────────────────
//  워커 스레드
// ─────────────────────────────────────────────
void *worker_thread_loop(void *arg) {
    int thread_id = *(int *)arg;
    free(arg);
    while (1) {
        thread_status[thread_id] = 0;
        update_top_win();
        int client_sock = dequeue_job();
        thread_status[thread_id] = 1;
        snprintf(thread_task[thread_id], sizeof(thread_task[thread_id]), "Processing client...");
        update_top_win();
        handle_client(client_sock, thread_id);
    }
    return NULL;
}

static void handle_ctl_client(int cfd) {
    IpcRequest req;
    IpcResponse resp;
    memset(&resp, 0, sizeof(resp));

    if (recv(cfd, &req, sizeof(req), MSG_WAITALL) <= 0) {
        close(cfd); return;
    }

    switch (req.cmd) {
        case CMD_SET_HW: {
            char week[MAX_WEEK_NAME] = {0};
            strncpy(week, req.payload, MAX_WEEK_NAME - 1);
            week[MAX_WEEK_NAME - 1] = '\0';
            week[strcspn(week, " \r\n")] = '\0';
            if (strlen(week) == 0) {
                resp.status = IPC_ERROR;
                snprintf(resp.data, IPC_MAX_PAYLOAD, "ERR|empty week name");
                break;
            }
            ctx_set_hw(&g_ctx, week);
            resp.status = IPC_OK;
            snprintf(resp.data, IPC_MAX_PAYLOAD, "OK|week=%s", g_ctx.current_hw);
            fprintf(stderr, "[ipc] 주차 전환 완료: %s\n", g_ctx.current_hw);
            break;
        }

        case CMD_SET_STUDENTS: {
            int n = atoi(req.payload);
            if (n <= 0 || n > MAX_STUDENTS) {
                resp.status = IPC_ERROR;
                snprintf(resp.data, IPC_MAX_PAYLOAD, "ERR|invalid count %d", n);
                break;
            }
            pthread_mutex_lock(&g_ctx.tracker_lock);
            g_ctx.total_students = (uint16_t)n;
            pthread_mutex_unlock(&g_ctx.tracker_lock);
            resp.status = IPC_OK;
            snprintf(resp.data, IPC_MAX_PAYLOAD, "OK|total=%d", n);
            fprintf(stderr, "[ipc] 수강생 수 설정: %d명\n", n);
            break;
        }

        case CMD_GET_TRACKER: {
            resp.status = IPC_OK;
            uint16_t snap_submitted, snap_total;
            uint32_t snap_records;
            char     snap_week[MAX_WEEK_NAME];

            pthread_mutex_lock(&g_ctx.tracker_lock);
            snap_submitted = g_ctx.submitted_count;
            snap_total     = g_ctx.total_students;
            pthread_mutex_unlock(&g_ctx.tracker_lock);

            pthread_rwlock_rdlock(&g_ctx.hw_lock);
            strncpy(snap_week, g_ctx.current_hw, MAX_WEEK_NAME - 1);
            snap_week[MAX_WEEK_NAME - 1] = '\0';
            pthread_rwlock_unlock(&g_ctx.hw_lock);

            pthread_mutex_lock(&g_ctx.csv_lock);
            snap_records = g_ctx.csv.total_records;
            pthread_mutex_unlock(&g_ctx.csv_lock);

            int pos = snprintf(resp.data, IPC_MAX_PAYLOAD,
                               "submitted=%u|total=%u|week=%s|records=%u|\n",
                               snap_submitted, snap_total, snap_week, snap_records);

            static const char *gnames[] = {"AC","WA","TLE","RE","CE"};
            pthread_mutex_lock(&g_ctx.tracker_lock);
            for (int i = 0; i < MAX_STUDENTS && pos < IPC_MAX_PAYLOAD - 24; i++) {
                if (g_ctx.students[i].student_id[0] == '\0') break;
                uint8_t r = g_ctx.students[i].best_result;
                const char *gs = (r < 5) ? gnames[r] : "UNK";
                pos += snprintf(resp.data + pos, IPC_MAX_PAYLOAD - pos,
                                "%s:%s,", g_ctx.students[i].student_id, gs);
            }
            pthread_mutex_unlock(&g_ctx.tracker_lock);
            break;
        }

        case CMD_GET_CSV_STAT: {
            pthread_mutex_lock(&g_ctx.csv_lock);
            uint32_t rec = g_ctx.csv.total_records;
            time_t   ts  = g_ctx.csv.last_write_ts;
            pthread_mutex_unlock(&g_ctx.csv_lock);

            char ts_str[32] = "never";
            if (ts > 0) {
                struct tm *tm = localtime(&ts);
                strftime(ts_str, sizeof(ts_str), "%Y-%m-%dT%H:%M:%S", tm);
            }
            resp.status = IPC_OK;
            snprintf(resp.data, IPC_MAX_PAYLOAD,
                     "records=%u|last_write=%s", rec, ts_str);
            break;
        }

        case CMD_TOGGLE_DIST: {
            pthread_mutex_lock(&g_ctx.state_lock);
            g_ctx.dist_paused = !g_ctx.dist_paused;
            int current = g_ctx.dist_paused;
            pthread_mutex_unlock(&g_ctx.state_lock);

            resp.status = IPC_OK;
            snprintf(resp.data, IPC_MAX_PAYLOAD, current ? "배포 차단됨 (OFF)" : "배포 허용됨 (ON)");
            break;
        }

        case CMD_STATUS:
        case CMD_PAUSE:
        case CMD_RESUME:
        case CMD_SET_TIMEOUT:
            resp.status = IPC_OK;
            snprintf(resp.data, IPC_MAX_PAYLOAD, "OK (legacy ignored)");
            break;

        default:
            resp.status = IPC_ERROR;
            snprintf(resp.data, IPC_MAX_PAYLOAD, "ERR|unknown command");
            break;
    }

    send(cfd, &resp, sizeof(resp), 0);
    close(cfd);
}

// ─────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────
int main() {
    ctx_init(&g_ctx);
    /* ── [0] 파일 로그 먼저 — 이 시점부터 모든 로그가 /tmp/ci_server.log에 기록됨 */
    ui_logfile_init();

    /* ── [1] 시그널 등록 */
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    struct sigaction sa;
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    /* ── [2] ncurses 초기화 — 실패해도 계속 실행 */
    const char *term = getenv("TERM");
    if (!term || strlen(term) == 0) {
        fprintf(stderr, "[WARN] TERM 미설정. ncurses 비활성화, stderr 모드로 동작.\n");
        fprintf(stderr, "       로그: tail -f /tmp/ci_server.log\n");
        fflush(stderr);
        /* ncurses 없이 소켓만 올림 — 아래 스레드/소켓 코드로 바로 점프 */
        goto start_server;
    }

    setlocale(LC_ALL, "");
    initscr();

    if (isendwin()) {
        fprintf(stderr, "[WARN] initscr() 실패. stderr 모드.\n");
        fflush(stderr);
        goto start_server;
    }

    cbreak();
    noecho();
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_PAIR_IDLE,    COLOR_GREEN,   -1);
        init_pair(COLOR_PAIR_RUNNING, COLOR_YELLOW,  -1);
        init_pair(COLOR_PAIR_ALERT,   COLOR_RED,     -1);
        init_pair(COLOR_PAIR_INFO,    COLOR_CYAN,    -1);
        init_pair(COLOR_PAIR_HEADER,  COLOR_BLACK,   COLOR_BLUE);
        init_pair(COLOR_PAIR_SUCCESS, COLOR_GREEN,   -1);
        init_pair(COLOR_PAIR_FAIL,    COLOR_MAGENTA, -1);
        init_pair(COLOR_PAIR_DIM,     COLOR_WHITE,   -1);
    } else {
        fprintf(stderr, "[WARN] 터미널이 색상 미지원. 흑백 모드.\n");
        fflush(stderr);
    }

    {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);

        if (max_y < NUM_THREADS + 4) {
            endwin();
            fprintf(stderr, "[FAIL] 터미널 높이 부족 (%d줄). 최소 %d줄 필요.\n",
                    max_y, NUM_THREADS + 4);
            fprintf(stderr, "       터미널을 크게 키운 후 다시 실행하세요.\n");
            fflush(stderr);
            goto start_server; /* ncurses 없이 계속 */
        }

        top_win    = newwin(NUM_THREADS + 2, max_x, 0, 0);
        bottom_win = newwin(max_y - (NUM_THREADS + 2), max_x, NUM_THREADS + 2, 0);

        if (!top_win || !bottom_win) {
            endwin();
            fprintf(stderr, "[FAIL] newwin() 실패.\n");
            fflush(stderr);
            goto start_server;
        }

        scrollok(bottom_win, TRUE);
    }

    /* 여기까지 오면 ncurses 완전히 준비됨 */
    ui_ncurses_ready();   /* 이제부터 ui_log가 bottom_win에 출력 */
    ui_diagnose();        /* 진단 정보를 파일+stderr에 덤프 */
    update_top_win();

start_server:
    /* ── [3] 초기 로그 — ncurses든 stderr든 반드시 찍힘 */
    ui_log_info("CI Server 시작 | port=%d | threads=%d\n", PORT, NUM_THREADS);
    ui_log_dim("로그 파일: tail -f /tmp/ci_server.log\n");
    ui_log_dim("종료: Ctrl+C\n\n");

    /* ── [4] 워커 스레드 — ncurses 초기화 완료 후 생성 */
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_status[i] = 0;
        int *id = malloc(sizeof(int));
        *id = i;
        pthread_t t;
        pthread_create(&t, NULL, worker_thread_loop, id);
    }

    /* ── [5] 소켓 설정 */
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        ui_log_alert("[FATAL] socket() 실패: %s\n", strerror(errno));
        if (g_ncurses_ok) endwin();
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ui_log_alert("[FATAL] bind() 실패: %s\n", strerror(errno));
        if (g_ncurses_ok) endwin();
        return 1;
    }

    listen(server_sock, 10);

    /* ── [5.1] ctl UNIX Domain Socket ── */
    int ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctl_fd < 0) { ui_log_alert("[FATAL] ctl socket\n"); return 1; }

    unlink(CTL_SOCK_PATH);

    struct sockaddr_un ctl_addr = {0};
    ctl_addr.sun_family = AF_UNIX;
    strncpy(ctl_addr.sun_path, CTL_SOCK_PATH, sizeof(ctl_addr.sun_path) - 1);

    if (bind(ctl_fd, (struct sockaddr *)&ctl_addr, sizeof(ctl_addr)) < 0) {
        ui_log_alert("[FATAL] ctl bind\n"); return 1;
    }
    chmod(CTL_SOCK_PATH, 0600);
    listen(ctl_fd, 8);

    /* ── [5.2] epoll 초기화 ── */
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) { ui_log_alert("epoll_create1\n"); return 1; }

    struct epoll_event ev;

    ev.events = EPOLLIN; ev.data.fd = server_sock;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &ev);

    ev.events = EPOLLIN; ev.data.fd = ctl_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctl_fd, &ev);

    ui_log_info("포트 %d 에서 대기 중...\n", PORT);

    /* ── [6] 메인 이벤트 루프 (epoll) ── */
    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            ui_log_fail("epoll_wait\n");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == server_sock) {
                int client_sock = accept(server_sock, NULL, NULL);
                if (client_sock < 0) continue;

                /* pause 상태면 즉시 거부 */
                if (atomic_load(&g_paused)) {
                    const char *msg = "서버가 일시 중지 상태입니다. 잠시 후 재시도하세요.\n";
                    send(client_sock, msg, strlen(msg), 0);
                    close(client_sock);
                    continue;
                }

                enqueue_job(client_sock);
            }
            else if (fd == ctl_fd) {
                int ccfd = accept(ctl_fd, NULL, NULL);
                if (ccfd < 0) continue;
                handle_ctl_client(ccfd); 
            }
        }
    }

    return 0;
}
