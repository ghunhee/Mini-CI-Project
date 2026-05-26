#define _XOPEN_SOURCE_EXTENDED 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <ncurses.h>
#include <pthread.h>
#include <locale.h>

#include "ipc_protocol.h"
#include "include/protocol.h"

/* ─── 전역 스냅샷 ─────────────────────────────────── */
typedef struct {
    char     week[MAX_WEEK_NAME];
    uint16_t submitted;
    uint16_t total;
    uint32_t csv_records;
    struct {
        char    id[MAX_STUDENT_ID];
        uint8_t result;   /* GradeResult */
    } entries[512];
    int entry_count;
} Snap;

static Snap              g_snap;
static pthread_mutex_t   snap_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int      tui_running = 1;

/* ─── 윈도우 핸들 ─────────────────────────────────── */
static WINDOW *w_header  = NULL;   /* 상단 상태바 1줄        */
static WINDOW *w_menu    = NULL;   /* 왼쪽 메뉴 패널         */
static WINDOW *w_tracker = NULL;   /* 오른쪽 진행률 패널     */
static WINDOW *w_log     = NULL;   /* 하단 로그 패널         */

/* ─── IPC 헬퍼 ────────────────────────────────────── */
static int ipc_send(IpcCommand cmd, const char *payload, IpcResponse *resp) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); return -1;
    }

    IpcRequest req;
    memset(&req, 0, sizeof(req));
    req.cmd = (uint8_t)cmd;
    if (payload) strncpy(req.payload, payload, IPC_MAX_PAYLOAD - 1);

    send(sock, &req, sizeof(req), 0);
    ssize_t n = recv(sock, resp, sizeof(*resp), MSG_WAITALL);
    close(sock);
    return (n == sizeof(*resp)) ? 0 : -1;
}

/* ─── 스냅샷 파싱 ─────────────────────────────────── */
static void parse_snap(const char *data, Snap *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->week, "week_?", MAX_WEEK_NAME - 1);

    const char *p = data;
    char key[32], val[64];
    while (p && *p && *p != '\n') {
        if (sscanf(p, "%31[^=]=%63[^|]|", key, val) == 2) {
            if (strcmp(key, "submitted") == 0)
                out->submitted   = (uint16_t)atoi(val);
            else if (strcmp(key, "total") == 0)
                out->total       = (uint16_t)atoi(val);
            else if (strcmp(key, "week") == 0)
                strncpy(out->week, val, MAX_WEEK_NAME - 1);
            else if (strcmp(key, "records") == 0)
                out->csv_records = (uint32_t)atoi(val);
        }
        p = strchr(p, '|');
        if (p) p++;
    }

    p = strchr(data, '\n');
    if (!p) return;
    p++;

    static const char *gnames[] = {"AC","WA","TLE","RE","CE",NULL};
    int idx = 0;
    while (*p && idx < 512) {
        char sid[MAX_STUDENT_ID] = {0};
        char gname[8] = {0};
        if (sscanf(p, "%15[^:]:%7[^,\n]", sid, gname) == 2) {
            strncpy(out->entries[idx].id, sid, MAX_STUDENT_ID - 1);
            out->entries[idx].result = 1; /* WA default */
            for (int g = 0; gnames[g]; g++) {
                if (strcmp(gname, gnames[g]) == 0) {
                    out->entries[idx].result = (uint8_t)g;
                    break;
                }
            }
            idx++;
        }
        p = strchr(p, ',');
        if (p) p++; else break;
    }
    out->entry_count = idx;
}

/* ─── 헤더 바 그리기 ──────────────────────────────── */
static void draw_header(void) {
    int cols = getmaxx(w_header);
    int pct  = g_snap.total > 0
               ? (int)(g_snap.submitted * 100 / g_snap.total) : 0;

    werase(w_header);
    wattron(w_header, COLOR_PAIR(6) | A_BOLD);
    mvwhline(w_header, 0, 0, ' ', cols);
    mvwprintw(w_header, 0, 1,
              " Mini CI Admin  |  Week: %-10s  |  %u / %u  (%d%%)  |  CSV: %u",
              g_snap.week, g_snap.submitted, g_snap.total, pct, g_snap.csv_records);
    wattroff(w_header, COLOR_PAIR(6) | A_BOLD);
    wrefresh(w_header);
}

/* ─── 진행률 패널 그리기 ──────────────────────────── */
static void draw_tracker(void) {
    int rows = getmaxy(w_tracker);
    int cols = getmaxx(w_tracker) - 4;

    werase(w_tracker);
    box(w_tracker, 0, 0);

    wattron(w_tracker, A_BOLD);
    mvwprintw(w_tracker, 1, 2, "Submission Progress");
    wattroff(w_tracker, A_BOLD);

    int pct  = g_snap.total > 0
               ? (int)(g_snap.submitted * 100 / g_snap.total) : 0;
    int fill = (cols * pct) / 100;

    mvwprintw(w_tracker, 2, 2, "[");
    wattron(w_tracker, COLOR_PAIR(2));
    for (int i = 0; i < fill; i++) waddch(w_tracker, ACS_CKBOARD);
    wattroff(w_tracker, COLOR_PAIR(2));
    wattron(w_tracker, COLOR_PAIR(3));
    for (int i = fill; i < cols; i++) waddch(w_tracker, '-');
    wattroff(w_tracker, COLOR_PAIR(3));
    wprintw(w_tracker, "] %u/%u %d%%",
            g_snap.submitted, g_snap.total, pct);

    static const char *gn[] = {"AC","WA","TLE","RE","CE"};
    int max_list = rows - 5;
    for (int i = 0; i < g_snap.entry_count && i < max_list; i++) {
        int pair = (g_snap.entries[i].result == 0) ? 2 : 4;
        uint8_t r = g_snap.entries[i].result < 5 ? g_snap.entries[i].result : 4;
        wattron(w_tracker, COLOR_PAIR(pair));
        mvwprintw(w_tracker, 4 + i, 2, "%-12s [%-3s]",
                  g_snap.entries[i].id, gn[r]);
        wattroff(w_tracker, COLOR_PAIR(pair));
    }

    wrefresh(w_tracker);
}

/* ─── 로그 패널에 메시지 출력 ────────────────────── */
static void log_msg(int color_pair, const char *msg) {
    wattron(w_log, COLOR_PAIR(color_pair));
    wprintw(w_log, "%s\n", msg);
    wattroff(w_log, COLOR_PAIR(color_pair));
    wrefresh(w_log);
}

/* ─── 폴링 스레드 ─────────────────────────────────── */
static void *poll_thread(void *arg) {
    (void)arg;
    while (tui_running) {
        IpcResponse resp;
        if (ipc_send(CMD_GET_TRACKER, NULL, &resp) == 0
            && resp.status == IPC_OK) {
            Snap tmp;
            parse_snap(resp.data, &tmp);

            IpcResponse csv_resp;
            if (ipc_send(CMD_GET_CSV_STAT, NULL, &csv_resp) == 0
                && csv_resp.status == IPC_OK) {
                uint32_t rec = 0;
                sscanf(csv_resp.data, "records=%u", &rec);
                tmp.csv_records = rec;
            }

            pthread_mutex_lock(&snap_mu);
            g_snap = tmp;
            pthread_mutex_unlock(&snap_mu);

            draw_header();
            draw_tracker();
        }
        sleep(1);
    }
    return NULL;
}

/* ─── 서브 입력창 (week 이름 / 숫자 입력용) ──────── */
static int popup_input(const char *prompt, char *out, int maxlen) {
    int rows = LINES, cols = COLS;
    int pw = 50, ph = 5;
    int py = rows / 2 - ph / 2, px = cols / 2 - pw / 2;

    WINDOW *pop = newwin(ph, pw, py, px);
    box(pop, 0, 0);
    wattron(pop, A_BOLD);
    mvwprintw(pop, 1, 2, "%s", prompt);
    wattroff(pop, A_BOLD);
    mvwprintw(pop, 3, 2, "> ");
    wrefresh(pop);

    echo();
    curs_set(1);
    mvwgetnstr(pop, 3, 4, out, maxlen - 1);
    noecho();
    curs_set(0);

    delwin(pop);
    touchwin(stdscr);
    refresh();
    draw_header();
    draw_tracker();
    return (strlen(out) > 0) ? 0 : -1;
}

/* ─── 메인 ────────────────────────────────────────── */
int main(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();

    init_pair(1, COLOR_WHITE,   COLOR_BLUE);    /* 헤더              */
    init_pair(2, COLOR_GREEN,   -1);            /* AC / 완료         */
    init_pair(3, COLOR_WHITE,   -1);            /* 미완료 bar        */
    init_pair(4, COLOR_RED,     -1);            /* WA/에러           */
    init_pair(5, COLOR_YELLOW,  -1);            /* 경고              */
    init_pair(6, COLOR_BLACK,   COLOR_CYAN);    /* 상단 헤더바       */
    init_pair(7, COLOR_BLACK,   COLOR_WHITE);   /* 선택된 메뉴 항목  */
    init_pair(8, COLOR_CYAN,    -1);            /* 정보              */

    int total_rows = LINES, total_cols = COLS;
    int menu_w   = 28;
    int track_w  = total_cols - menu_w;
    int top_h    = 1;
    int bot_h    = 6;
    int mid_h    = total_rows - top_h - bot_h;

    w_header  = newwin(top_h,  total_cols, 0,     0);
    w_menu    = newwin(mid_h,  menu_w,     top_h, 0);
    w_tracker = newwin(mid_h,  track_w,    top_h, menu_w);
    w_log     = newwin(bot_h,  total_cols, top_h + mid_h, 0);

    scrollok(w_log, TRUE);
    keypad(w_menu, TRUE);

    /* 메뉴 항목 정의 */
    typedef struct { const char *label; IpcCommand cmd; int needs_input; } MenuItem;
    MenuItem menu[] = {
        { "  Status",          CMD_STATUS,       0 },
        { "  Set Week...",     CMD_SET_HW,       1 },
        { "  Set Students...", CMD_SET_STUDENTS, 1 },
        { "  Pause",           CMD_PAUSE,        0 },
        { "  Resume",          CMD_RESUME,       0 },
        { "  Export CSV",      CMD_EXPORT_CSV,   0 },
        { "  CSV Status",      CMD_GET_CSV_STAT, 0 },
    };
    int menu_count = (int)(sizeof(menu) / sizeof(menu[0]));
    int selected   = 0;

    /* 초기 스냅샷 */
    IpcResponse init_resp;
    if (ipc_send(CMD_GET_TRACKER, NULL, &init_resp) == 0)
        parse_snap(init_resp.data, &g_snap);
    draw_header();
    draw_tracker();

    /* 폴링 스레드 시작 */
    pthread_t ptid;
    pthread_create(&ptid, NULL, poll_thread, NULL);

    /* ─── 메인 루프 ─── */
    while (1) {
        /* 메뉴 그리기 */
        werase(w_menu);
        box(w_menu, 0, 0);
        wattron(w_menu, A_BOLD);
        mvwprintw(w_menu, 1, 2, "Commands");
        wattroff(w_menu, A_BOLD);
        mvwhline(w_menu, 2, 1, ACS_HLINE, menu_w - 2);

        for (int i = 0; i < menu_count; i++) {
            if (i == selected) {
                wattron(w_menu, COLOR_PAIR(7) | A_BOLD);
                mvwprintw(w_menu, 3 + i, 1, "%-*s", menu_w - 2, menu[i].label);
                wattroff(w_menu, COLOR_PAIR(7) | A_BOLD);
            } else {
                mvwprintw(w_menu, 3 + i, 1, "%-*s", menu_w - 2, menu[i].label);
            }
        }

        int hint_row = getmaxy(w_menu) - 2;
        wattron(w_menu, COLOR_PAIR(5));
        mvwprintw(w_menu, hint_row, 2, "ENTER:run  q:quit");
        wattroff(w_menu, COLOR_PAIR(5));

        wrefresh(w_menu);

        int ch = wgetch(w_menu);

        if (ch == 'q' || ch == KEY_F(10)) break;

        if (ch == KEY_UP) {
            selected = (selected - 1 + menu_count) % menu_count;
            continue;
        }
        if (ch == KEY_DOWN) {
            selected = (selected + 1) % menu_count;
            continue;
        }

        if (ch == '\n' || ch == KEY_ENTER) {
            char input[IPC_MAX_PAYLOAD] = {0};
            const char *payload = NULL;

            if (menu[selected].needs_input) {
                const char *prompt =
                    (menu[selected].cmd == CMD_SET_HW)
                    ? "Enter week name (e.g. week_3):"
                    : "Enter student count (e.g. 30):";

                if (popup_input(prompt, input, sizeof(input)) < 0)
                    continue;
                payload = input;
            }

            IpcResponse resp;
            memset(&resp, 0, sizeof(resp));
            int ret = ipc_send(menu[selected].cmd, payload, &resp);

            char msg[IPC_MAX_PAYLOAD + 32];
            if (ret < 0) {
                snprintf(msg, sizeof(msg), "[ERR] Server not reachable");
                log_msg(4, msg);
            } else if (resp.status == IPC_OK) {
                snprintf(msg, sizeof(msg), "[OK]  %s", resp.data);
                log_msg(2, msg);

                /* set-hw 성공 시 스냅샷 week 즉시 갱신 */
                if (menu[selected].cmd == CMD_SET_HW && payload) {
                    pthread_mutex_lock(&snap_mu);
                    strncpy(g_snap.week, payload, MAX_WEEK_NAME - 1);
                    pthread_mutex_unlock(&snap_mu);
                    draw_header();
                }
            } else {
                snprintf(msg, sizeof(msg), "[ERR] %s", resp.data);
                log_msg(4, msg);
            }
        }
    }

    tui_running = 0;
    pthread_join(ptid, NULL);
    endwin();
    return 0;
}
