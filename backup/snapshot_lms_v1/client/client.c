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

#include "../include/protocol.h"

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

/*
 * send_source()
 * — [기능 1] SubmitHeader 에 student_id 를 채워 서버로 전송.
 * — 헤더 → 페이로드 → ResultHeader 수신 → ncurses 상태바 갱신.
 */
static int send_source(const char *student_id,
                       const char *filepath,
                       ResultHeader *out_res) {
    /* 1) 파일 읽기 */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) { perror("open src"); return -1; }

    struct stat st;
    fstat(fd, &st);
    off_t fsize = st.st_size;

    if (fsize == 0 || fsize > 1024 * 1024) {
        fprintf(stderr, "File size error: %ld bytes\n", (long)fsize);
        close(fd); return -1;
    }
    uint8_t *payload = malloc(fsize);
    if (!payload) { close(fd); return -1; }

    ssize_t nread = read(fd, payload, fsize);
    close(fd);
    if (nread != fsize) {
        fprintf(stderr, "Incomplete file read\n");
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
    bname = bname ? bname + 1 : filepath;
    strncpy(hdr.filename, bname, MAX_FILENAME - 1);

    /* 3) 서버 연결 */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &srv.sin_addr);

    if (connect(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("connect");
        free(payload); close(sock); return -1;
    }

    /* 4) 헤더 전송 */
    if (send(sock, &hdr, sizeof(hdr), 0) != sizeof(hdr)) {
        perror("send header");
        free(payload); close(sock); return -1;
    }

    /* 5) 페이로드 분할 전송 */
    size_t sent = 0;
    while (sent < (size_t)fsize) {
        ssize_t n = send(sock, payload + sent, fsize - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
    free(payload);

    /* 6) ResultHeader 수신 */
    memset(out_res, 0, sizeof(*out_res));
    recv(sock, out_res, sizeof(*out_res), MSG_WAITALL);
    on_result_received(out_res);
    close(sock);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file1.c> [file2.c ...]\n", argv[0]);
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
