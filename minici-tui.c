#define _XOPEN_SOURCE_EXTENDED 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <ncurses.h>
#include <pthread.h>
#include <locale.h>
#include <time.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/eventfd.h>

#include "ipc_protocol.h"
#include "include/protocol.h"

/* ─── 전역 스냅샷 ─── */
typedef struct {
    char     week[MAX_WEEK_NAME];
    uint16_t submitted;
    uint16_t total;
    uint32_t csv_records;
    struct { char id[MAX_STUDENT_ID]; uint8_t result; } entries[512];
    int entry_count;
} Snap;

static Snap            g_snap;
static pthread_mutex_t snap_mu     = PTHREAD_MUTEX_INITIALIZER;
static volatile int    tui_running = 1;

/* 교수가 설정한 부가 정보 (TUI 로컬) */
static char g_deadline[64]  = "미설정";
static char g_testsh[256]   = "";

/*
 * Locking policy:
 * 1. Lock order is always ui_mu -> snap_mu.
 * 2. Never call ncurses, log_msg(), safe_draw_all(), popup_*()
 *    while holding snap_mu.
 * 3. popup_* must be called while holding ui_mu.
 */
static char g_staged_week[MAX_WEEK_NAME] = "";
static char g_staged_testsh[256] = "";
static pthread_mutex_t ui_mu   = PTHREAD_MUTEX_INITIALIZER;

static int tui_valid_week_name(const char *s) {
    size_t len;
    size_t i;

    if (!s) return 0;

    len = strlen(s);
    if (len == 0 || len >= MAX_WEEK_NAME) return 0;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '_' && c != '-') return 0;
    }

    return 1;
}

/* ─── 윈도우 ─── */
static WINDOW *w_hdr  = NULL;
static WINDOW *w_menu = NULL;
static WINDOW *w_info = NULL;
static WINDOW *w_log  = NULL;

/* ─── IPC ─── */
static int ipc_send(IpcCommand cmd, const char *payload, IpcResponse *resp) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un a; memset(&a,0,sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, IPC_SOCK_PATH, sizeof(a.sun_path)-1);
    if (connect(sock,(struct sockaddr*)&a,sizeof(a)) < 0) { close(sock); return -1; }
    IpcRequest req; memset(&req,0,sizeof(req));
    req.cmd = (uint8_t)cmd;
    if (payload) strncpy(req.payload, payload, IPC_MAX_PAYLOAD-1);
    send(sock, &req, sizeof(req), 0);
    ssize_t n = recv(sock, resp, sizeof(*resp), MSG_WAITALL);
    close(sock);
    return (n == sizeof(*resp)) ? 0 : -1;
}

/* ─── 파싱 ─── */
static void parse_snap(const char *data, Snap *out) {
    memset(out,0,sizeof(*out));
    strncpy(out->week,"week_?",MAX_WEEK_NAME-1);
    const char *p = data; char k[32],v[64];
    while (p && *p && *p != '\n') {
        if (sscanf(p,"%31[^=]=%63[^|]|",k,v)==2) {
            if (!strcmp(k,"submitted")) out->submitted=(uint16_t)atoi(v);
            else if (!strcmp(k,"total")) out->total=(uint16_t)atoi(v);
            else if (!strcmp(k,"week")) strncpy(out->week,v,MAX_WEEK_NAME-1);
            else if (!strcmp(k,"records")) out->csv_records=(uint32_t)atoi(v);
        }
        p=strchr(p,'|'); if(p) p++;
    }
    p=strchr(data,'\n'); if(!p) return; p++;
    static const char *gn[]={"AC","WA","TLE","RE","CE",NULL};
    int idx=0;
    while(*p && idx<512) {
        char sid[MAX_STUDENT_ID]={0}, gname[8]={0};
        if (sscanf(p,"%15[^:]:%7[^,\n]",sid,gname)==2) {
            strncpy(out->entries[idx].id,sid,MAX_STUDENT_ID-1);
            out->entries[idx].result=1;
            for(int g=0;gn[g];g++) if(!strcmp(gname,gn[g])){out->entries[idx].result=(uint8_t)g;break;}
            idx++;
        }
        p=strchr(p,','); if(p) p++; else break;
    }
    out->entry_count=idx;
}

/* ─── 헤더 ─── */
static void draw_hdr(void) {
    int cols;
    int pct;
    Snap snap;
    char local_deadline[64];

    cols = getmaxx(w_hdr);

    pthread_mutex_lock(&snap_mu);
    snap = g_snap;
    snprintf(local_deadline, sizeof(local_deadline), "%s", g_deadline);
    pthread_mutex_unlock(&snap_mu);

    pct = snap.total > 0 ? (int)(snap.submitted * 100 / snap.total) : 0;

    werase(w_hdr);
    wbkgd(w_hdr, COLOR_PAIR(1));
    wattron(w_hdr, A_BOLD);
    mvwhline(w_hdr, 0, 0, ' ', cols);

    mvwprintw(w_hdr, 0, 2, "Mini CI Dashboard");

    if (cols > 38) {
        wattron(w_hdr, COLOR_PAIR(5) | A_REVERSE | A_BOLD);
        mvwprintw(w_hdr, 0, 24, " ACTIVE: %.16s ", snap.week);
        wattroff(w_hdr, COLOR_PAIR(5) | A_REVERSE | A_BOLD);
    }

    if (cols > 68) {
        mvwprintw(w_hdr, 0, 45, "Submit: %u/%u (%d%%) CSV: %u",
                  snap.submitted, snap.total, pct, snap.csv_records);
    }

    if (cols > 100) {
        mvwprintw(w_hdr, 0, cols - 32, "Deadline: %.20s",
                  local_deadline[0] ? local_deadline : "Not Set");
    }

    wattroff(w_hdr, A_BOLD);
    wrefresh(w_hdr);
}

/* ─── 진행률 패널 ─── */
static void draw_info(void) {
    int rows;
    int width;
    int pct;
    int bar_width;
    int fill;
    int maxr;
    int i;
    Snap snap;
    char local_week[MAX_WEEK_NAME];
    char local_testsh[256];
    char local_active_testsh[256];
    char suffix[64];
    static const char *gn[] = {"AC", "WA", "TLE", "RE", "CE"};

    rows = getmaxy(w_info);
    width = getmaxx(w_info) - 4;

    werase(w_info);
    box(w_info, 0, 0);

    if (rows < 12 || getmaxx(w_info) < 40) {
        mvwprintw(w_info, 1, 2, "Window too small");
        wrefresh(w_info);
        return;
    }

    pthread_mutex_lock(&snap_mu);
    snap = g_snap;
    snprintf(local_week, sizeof(local_week), "%s", g_staged_week);
    snprintf(local_testsh, sizeof(local_testsh), "%s", g_staged_testsh);
    snprintf(local_active_testsh, sizeof(local_active_testsh), "%s", g_testsh);
    pthread_mutex_unlock(&snap_mu);

    wattron(w_info, A_BOLD | COLOR_PAIR(3));
    mvwprintw(w_info, 1, 2, "[STAGING] Pending Changes");
    wattroff(w_info, A_BOLD | COLOR_PAIR(3));

    if (local_week[0]) {
        mvwprintw(w_info, 2, 4, "Week   : %.24s", local_week);
    } else {
        wattron(w_info, A_DIM);
        mvwprintw(w_info, 2, 4, "Week   : (not set)");
        wattroff(w_info, A_DIM);
    }

    if (local_testsh[0]) {
        mvwprintw(w_info, 3, 4, "Script : %.40s", local_testsh);
    } else {
        wattron(w_info, A_DIM);
        mvwprintw(w_info, 3, 4, "Script : (not set)");
        wattroff(w_info, A_DIM);
    }

    mvwhline(w_info, 5, 1, ACS_HLINE, getmaxx(w_info) - 2);

    wattron(w_info, A_BOLD);
    mvwprintw(w_info, 6, 2, "[ACTIVE] Real-time Submissions (%.16s)", snap.week);
    wattroff(w_info, A_BOLD);

    pct = snap.total > 0 ? (int)(snap.submitted * 100 / snap.total) : 0;
    snprintf(suffix, sizeof(suffix), "] %u/%u  %d%%", snap.submitted, snap.total, pct);

    bar_width = width - 1 - (int)strlen(suffix);
    if (bar_width < 0) bar_width = 0;

    fill = bar_width > 0 ? (bar_width * pct) / 100 : 0;

    mvwprintw(w_info, 7, 2, "[");
    wattron(w_info, COLOR_PAIR(2));
    for (i = 0; i < fill; i++) waddch(w_info, ACS_CKBOARD);
    wattroff(w_info, COLOR_PAIR(2));

    wattron(w_info, COLOR_PAIR(3));
    for (i = fill; i < bar_width; i++) waddch(w_info, '-');
    wattroff(w_info, COLOR_PAIR(3));

    wprintw(w_info, "%s", suffix);

    if (local_active_testsh[0]) {
        mvwprintw(w_info, 8, 2, "Active Script: %.48s", local_active_testsh);
    }

    maxr = rows - 11;
    if (maxr < 0) maxr = 0;

    for (i = 0; i < snap.entry_count && i < maxr; i++) {
        uint8_t r = snap.entries[i].result < 5 ? snap.entries[i].result : 4;
        int pair = (r == 0) ? 2 : 4;

        wattron(w_info, COLOR_PAIR(pair));
        mvwprintw(w_info, 10 + i, 2, "%-14s [%-3s]", snap.entries[i].id, gn[r]);
        wattroff(w_info, COLOR_PAIR(pair));
    }

    wrefresh(w_info);
}

static void safe_draw_all(void) {
    pthread_mutex_lock(&ui_mu);
    draw_hdr();
    draw_info();
    pthread_mutex_unlock(&ui_mu);
}

/* ─── 로그 ─── */
static void log_msg(int pair, const char *msg) {
    pthread_mutex_lock(&ui_mu);
    wattron(w_log, COLOR_PAIR(pair));
    wprintw(w_log, "%s\n", msg);
    wattroff(w_log, COLOR_PAIR(pair));
    wrefresh(w_log);
    pthread_mutex_unlock(&ui_mu);
}

/* ─── 폴링 스레드 (FIX-9: ncurses 직접 렌더링 제거) ─── */
int g_poll_efd = -1;

static void *poll_fn(void *a) {
    (void)a;
    while(tui_running) {
        IpcResponse r;
        if(ipc_send(CMD_GET_TRACKER,NULL,&r)==0 && r.status==IPC_OK) {
            Snap t; parse_snap(r.data,&t);
            IpcResponse cr;
            if(ipc_send(CMD_GET_CSV_STAT,NULL,&cr)==0 && cr.status==IPC_OK) {
                uint32_t rec=0; sscanf(cr.data,"records=%u",&rec); t.csv_records=rec;
            }
            pthread_mutex_lock(&snap_mu); 
            g_snap=t; 
            pthread_mutex_unlock(&snap_mu);
            
            /* 메인 루프에 이벤트 시그널 전송 */
            if (g_poll_efd >= 0) {
                uint64_t u = 1;
                write(g_poll_efd, &u, sizeof(u));
            }
        }
        sleep(1);
    }
    return NULL;
}

/* ─── 팝업 입력 ─── */
static int popup_input(const char *title, const char *hint, char *out, int maxlen) {
    int pw=56, ph=7;
    int py=LINES/2-ph/2, px=COLS/2-pw/2;
    WINDOW *pop=newwin(ph,pw,py,px);
    box(pop,0,0);
    wattron(pop,COLOR_PAIR(6)|A_BOLD);
    mvwprintw(pop,1,2,"%s",title);
    wattroff(pop,COLOR_PAIR(6)|A_BOLD);
    wattron(pop,COLOR_PAIR(5));
    mvwprintw(pop,3,2,"예시: %s",hint);
    wattroff(pop,COLOR_PAIR(5));
    mvwprintw(pop,5,2,"> ");
    wrefresh(pop);
    echo(); curs_set(1);
    mvwgetnstr(pop, 5, 4, out, maxlen - 1);
    noecho();
    curs_set(0);

    delwin(pop);
    touchwin(stdscr);
    refresh();

    /*
     * Do not call draw_hdr() or draw_info() here.
     * Caller owns redraw through safe_draw_all().
     */
    return (out[0] != '\0') ? 0 : -1;
}

/* ─── 내장 파일 탐색기 ─── */
static int popup_file_picker(char *out_path, int maxlen) {
    char orig_cwd[1024], cwd[1024];
    if (!getcwd(orig_cwd, sizeof(orig_cwd))) return -1;
    
    /* 관리자용 스크립트 전용 폴더가 없으면 만들고 그 안으로 진입 */
    mkdir("grading_scripts", 0755);
    chdir("grading_scripts");
    getcwd(cwd, sizeof(cwd));
    int sel = 0, ret = -1;

    while(1) {
        DIR *d = opendir(cwd);
        if(!d) break;
        struct dirent *dir;
        char items[256][256];
        int is_dir[256];
        int count = 0;

        strcpy(items[count], "..");
        is_dir[count] = 1;
        count++;

        while ((dir = readdir(d)) != NULL && count < 256) {
            if(dir->d_name[0] == '.') continue;
            strncpy(items[count], dir->d_name, 255);
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", cwd, dir->d_name);
            struct stat st;
            if (stat(full, &st) == 0) {
                is_dir[count] = S_ISDIR(st.st_mode);
            } else {
                is_dir[count] = 0;
            }
            count++;
        }
        closedir(d);

        werase(w_info);
        box(w_info, 0, 0);
        wattron(w_info, COLOR_PAIR(6)|A_BOLD);
        mvwprintw(w_info, 1, 2, "[ 파일 탐색기 ]");
        wattroff(w_info, COLOR_PAIR(6)|A_BOLD);
        wattron(w_info, COLOR_PAIR(5));
        mvwprintw(w_info, 2, 2, "%s", cwd);
        wattroff(w_info, COLOR_PAIR(5));

        int maxr = getmaxy(w_info) - 6;
        if (maxr < 1) maxr = 1;
        int page_start = (sel / maxr) * maxr;

        for(int i = 0; i < maxr && i + page_start < count; i++) {
            int idx = i + page_start;
            if(idx == sel) {
                wattron(w_info, COLOR_PAIR(7)|A_BOLD);
            }
            mvwprintw(w_info, 4+i, 2, "%s %-30s", is_dir[idx] ? "[폴더]" : "[파일]", items[idx]);
            if(idx == sel) {
                wattroff(w_info, COLOR_PAIR(7)|A_BOLD);
            }
        }
        wattron(w_info, COLOR_PAIR(8));
        mvwprintw(w_info, getmaxy(w_info)-2, 2, "방향키: 이동 | Enter: 선택(들어가기) | q: 취소");
        wattroff(w_info, COLOR_PAIR(8));
        wrefresh(w_info);

        int ch = wgetch(w_menu);
        if(ch == 'q' || ch == 27) { ret = -1; break; }
        if(ch == KEY_UP && sel > 0) sel--;
        if(ch == KEY_DOWN && sel < count-1) sel++;
        if(ch == '\n' || ch == KEY_ENTER) {
            char next[1024];
            snprintf(next, sizeof(next), "%s/%s", cwd, items[sel]);
            if(is_dir[sel]) {
                chdir(next);
                getcwd(cwd, sizeof(cwd));
                sel = 0;
            } else {
                strncpy(out_path, next, maxlen-1);
                ret = 0;
                break;
            }
        }
    }
    chdir(orig_cwd);
    werase(w_info);
    touchwin(stdscr);
    refresh();

    /*
     * No raw draw_hdr()/draw_info() here.
     * The caller will redraw with safe_draw_all().
     */
    ret = (out_path && out_path[0] != '\0') ? 0 : -1;
    return ret;
}

/* ─── test.sh 배포: submissions/현재주차/ 에 복사 ─── */
static void distribute_testsh(void) {
    if(g_testsh[0]=='\0') { log_msg(4,"[오류] test.sh 경로가 설정되지 않았습니다."); return; }
    char dst[512];
    snprintf(dst,sizeof(dst),"submissions/%s/test.sh",g_snap.week);

    char cmd[600];
    snprintf(cmd,sizeof(cmd),"mkdir -p submissions/%s && cp \"%s\" \"%s\"",
             g_snap.week, g_testsh, dst);
    int ret = system(cmd);

    char msg[300];
    if(ret==0) snprintf(msg,sizeof(msg),"[배포 완료] %s → %s", g_testsh, dst);
    else       snprintf(msg,sizeof(msg),"[배포 실패] 파일을 확인하세요: %s", g_testsh);
    log_msg(ret==0?2:4, msg);
}

/* ─── 메뉴 항목 ─── */
typedef struct {
    const char *icon;
    const char *label;
    const char *desc;
    int         action;   /* 0=IPC, 1=deadline, 2=testsh_path, 3=testsh_dist, 4=status */
    IpcCommand  cmd;
    int         needs_input;
} MenuItem;

#define ACT_IPC          0
#define ACT_DL           1
#define ACT_TSDIST       3
#define ACT_STAGE_WEEK   4
#define ACT_STAGE_SCRIPT 5
#define ACT_DEPLOY       6

static MenuItem MENU[] = {
    {"[현황]", "서버 상태 조회",      "현재 워커/큐 상태를 확인합니다",         ACT_IPC,          CMD_STATUS,       0},
    {"[준비]", "1. Set Target Week",  "스테이징: 전환할 주차 이름 입력",        ACT_STAGE_WEEK,   0,                0},
    {"[준비]", "2. Select test.sh",   "스테이징: 스크립트 경로 선택",           ACT_STAGE_SCRIPT, 0,                0},
    {"[적용]", "3. Apply to Server",  "설정된 주차명(Week)을 서버에 반영",      ACT_DEPLOY,       0,                0},
    {"[인원]", "수강생 수 설정",      "전체 수강생 수를 입력합니다 (예: 30)",   ACT_IPC,          CMD_SET_STUDENTS, 1},
    {"[기한]", "제출 기한 설정",      "마감 일시를 표시합니다 (예: 5/31 23:59)", ACT_DL,          0,                1},
    {"[배포]", "test.sh 학생 배포",   "지정된 test.sh를 제출 폴더에 복사합니다",ACT_TSDIST,       0,                0},
    {"[정지]", "채점 일시정지",       "새 제출을 잠시 받지 않습니다",           ACT_IPC,          CMD_PAUSE,        0},
    {"[재개]", "채점 재개",           "제출 접수를 다시 시작합니다",            ACT_IPC,          CMD_RESUME,       0},
    {"[배포]", "파일 배포 On/Off",    "학생들의 파일 다운로드를 켜고 끕니다",   ACT_IPC,          CMD_TOGGLE_DIST,  0},
    {"[CSV]",  "성적표 즉시 저장",    "results.csv 를 지금 flush 합니다",       ACT_IPC,          CMD_EXPORT_CSV,   0},
    {"[CSV]",  "성적표 저장 현황",    "CSV 누적 레코드 수와 마지막 저장 시각",  ACT_IPC,          CMD_GET_CSV_STAT, 0},
};
static const int MENU_CNT = (int)(sizeof(MENU)/sizeof(MENU[0]));

/* ─── 메인 ─── */
int main(void) {
    setlocale(LC_ALL,"");
    initscr(); cbreak(); noecho(); keypad(stdscr,TRUE); curs_set(0);
    start_color(); use_default_colors();
    init_pair(1, COLOR_WHITE,  COLOR_BLUE);
    init_pair(2, COLOR_GREEN,  -1);
    init_pair(3, COLOR_WHITE,  -1);
    init_pair(4, COLOR_RED,    -1);
    init_pair(5, COLOR_YELLOW, -1);
    init_pair(6, COLOR_BLACK,  COLOR_CYAN);
    init_pair(7, COLOR_BLACK,  COLOR_WHITE);
    init_pair(8, COLOR_CYAN,   -1);

    int R=LINES, C=COLS;
    int menu_w=36, info_w=C-menu_w;
    int top_h=1, bot_h=5, mid_h=R-top_h-bot_h;

    w_hdr  = newwin(top_h, C,      0,              0);
    w_menu = newwin(mid_h, menu_w, top_h,          0);
    w_info = newwin(mid_h, info_w, top_h,          menu_w);
    w_log  = newwin(bot_h, C,      top_h+mid_h,   0);
    scrollok(w_log,TRUE); keypad(w_menu,TRUE);

    /* 초기 스냅샷 */
    IpcResponse ir; memset(&ir,0,sizeof(ir));
    if(ipc_send(CMD_GET_TRACKER,NULL,&ir)==0) parse_snap(ir.data,&g_snap);
    safe_draw_all();

    g_poll_efd = eventfd(0, EFD_NONBLOCK);
    wtimeout(w_menu, 100);

    pthread_t ptid; pthread_create(&ptid,NULL,poll_fn,NULL);

    int sel=0;
    int dirty_menu = 1;
    while(1) {
        if (g_poll_efd >= 0) {
            uint64_t u;
            if (read(g_poll_efd, &u, sizeof(u)) > 0) {
                safe_draw_all();
            }
        }

        if (dirty_menu) {
            /* 메뉴 그리기 */
            pthread_mutex_lock(&ui_mu);
            werase(w_menu); box(w_menu,0,0);
            wattron(w_menu,COLOR_PAIR(6)|A_BOLD);
            mvwprintw(w_menu,1,2,"  Mini CI 관리 메뉴");
            wattroff(w_menu,COLOR_PAIR(6)|A_BOLD);
            mvwhline(w_menu,2,1,ACS_HLINE,menu_w-2);

            for(int i=0;i<MENU_CNT;i++){
                if(i==sel){
                    wattron(w_menu,COLOR_PAIR(7)|A_BOLD);
                    mvwprintw(w_menu,3+i,1," %-*s",menu_w-3,MENU[i].label);
                    wattroff(w_menu,COLOR_PAIR(7)|A_BOLD);
                    
                    werase(w_info); box(w_info,0,0);
                    wattron(w_info,COLOR_PAIR(5)|A_BOLD);
                    mvwprintw(w_info,1,2,"%s %s",MENU[i].icon,MENU[i].label);
                    wattroff(w_info,COLOR_PAIR(5)|A_BOLD);
                    mvwprintw(w_info,2,2,"%s",MENU[i].desc);
                    /* 진행률 등 나머지 */
                    draw_info();
                } else {
                    wattron(w_menu,COLOR_PAIR(8));
                    mvwprintw(w_menu,3+i,1," %s ",MENU[i].icon);
                    wattroff(w_menu,COLOR_PAIR(8));
                    mvwprintw(w_menu,3+i,7," %s",MENU[i].label);
                }
            }
            wattron(w_menu,COLOR_PAIR(5));
            mvwprintw(w_menu,getmaxy(w_menu)-2,2,"위아래: 이동  Enter: 실행  q: 종료");
            wattroff(w_menu,COLOR_PAIR(5));
            wrefresh(w_menu);
            pthread_mutex_unlock(&ui_mu);
            dirty_menu = 0;
        }

        int ch=wgetch(w_menu);
        if (ch == ERR) continue;

        dirty_menu = 1;
        if(ch=='q'||ch==KEY_F(10)) break;
        if(ch==KEY_UP)   { sel=(sel-1+MENU_CNT)%MENU_CNT; continue; }
        if(ch==KEY_DOWN) { sel=(sel+1)%MENU_CNT; continue; }

        if(ch=='\n'||ch==KEY_ENTER) {
            MenuItem *m=&MENU[sel];
            char input[IPC_MAX_PAYLOAD]={0};
            char msg[IPC_MAX_PAYLOAD+64]={0};

            if (m->action == ACT_STAGE_WEEK) {
                int pop_res;

                pthread_mutex_lock(&ui_mu);
                pop_res = popup_input("Set Target Week", "week_3", input, sizeof(input));
                pthread_mutex_unlock(&ui_mu);

                if (pop_res == 0) {
                    input[strcspn(input, "\r\n")] = '\0';

                    if (!tui_valid_week_name(input)) {
                        log_msg(4, "[ERROR] Week name allows only letters, digits, '_' and '-'.");
                    } else {
                        pthread_mutex_lock(&snap_mu);
                        snprintf(g_staged_week, sizeof(g_staged_week), "%s", input);
                        pthread_mutex_unlock(&snap_mu);

                        log_msg(2, "[STAGING] Target week staged.");
                    }
                }

                safe_draw_all();
                continue;
            }

            if (m->action == ACT_STAGE_SCRIPT) {
                char picked[1024] = {0};
                int pop_res;

                pthread_mutex_lock(&ui_mu);
                pop_res = popup_file_picker(picked, sizeof(picked));
                pthread_mutex_unlock(&ui_mu);

                if (pop_res == 0) {
                    pthread_mutex_lock(&snap_mu);
                    snprintf(g_staged_testsh, sizeof(g_staged_testsh), "%s", picked);
                    pthread_mutex_unlock(&snap_mu);

                    log_msg(2, "[STAGING] test.sh path staged.");
                } else {
                    log_msg(4, "[CANCEL] Script selection cancelled.");
                }

                safe_draw_all();
                continue;
            }

            if (m->action == ACT_DEPLOY) {
                char target_week[MAX_WEEK_NAME] = {0};
                IpcResponse resp;
                int ret;

                memset(&resp, 0, sizeof(resp));

                pthread_mutex_lock(&snap_mu);
                snprintf(target_week, sizeof(target_week), "%s", g_staged_week);
                pthread_mutex_unlock(&snap_mu);

                if (target_week[0] == '\0') {
                    log_msg(4, "[WARN] No staged week. Set target week first.");
                    safe_draw_all();
                    continue;
                }

                ret = ipc_send(CMD_SET_HW, target_week, &resp);

                if (ret < 0) {
                    log_msg(4, "[ERROR] Cannot connect to server.");
                } else if (resp.status == IPC_OK) {
                    pthread_mutex_lock(&snap_mu);
                    if (g_staged_testsh[0] != '\0') {
                        snprintf(g_testsh, sizeof(g_testsh), "%s", g_staged_testsh);
                    }

                    /*
                     * Do not modify g_snap here.
                     * poll_fn will fetch the fresh server snapshot for the new week.
                     * This prevents ghost data from the previous week.
                     */
                    g_staged_week[0] = '\0';
                    g_staged_testsh[0] = '\0';

                    pthread_mutex_unlock(&snap_mu);

                    snprintf(msg, sizeof(msg),
                             "[SUCCESS] Week applied to server: %s", resp.data);
                    log_msg(2, msg);
                } else {
                    snprintf(msg, sizeof(msg),
                             "[ERROR] Server rejected request: %s", resp.data);
                    log_msg(4, msg);
                }

                safe_draw_all();
                continue;
            }

            if(m->action==ACT_DL) {
                int pop_res;
                pthread_mutex_lock(&ui_mu);
                pop_res = popup_input("제출 기한 설정","2026-05-31 23:59",input,sizeof(input));
                pthread_mutex_unlock(&ui_mu);
                if(pop_res==0)
                    strncpy(g_deadline,input,sizeof(g_deadline)-1);
                safe_draw_all();
                continue;
            }
            if(m->action==ACT_TSDIST) {
                distribute_testsh(); 
                safe_draw_all();
                continue;
            }

            /* ACT_IPC */
            if(m->needs_input) {
                const char *hint = "30";
                const char *title= "수강생 수 입력 (예: 30)";
                int pop_res;
                pthread_mutex_lock(&ui_mu);
                pop_res = popup_input(title,hint,input,sizeof(input));
                pthread_mutex_unlock(&ui_mu);
                if(pop_res<0) { safe_draw_all(); continue; }
            }
            IpcResponse resp; memset(&resp,0,sizeof(resp));
            int ret=ipc_send(m->cmd, m->needs_input?input:NULL, &resp);
            if(ret<0) {
                log_msg(4,"[오류] 서버에 연결할 수 없습니다. 서버가 실행 중인지 확인하세요.");
            } else if(resp.status==IPC_OK) {
                snprintf(msg,sizeof(msg),"[완료] %s",resp.data);
                log_msg(2,msg);
            } else {
                snprintf(msg,sizeof(msg),"[실패] %s",resp.data);
                log_msg(4,msg);
            }
            safe_draw_all();
        }
    }

    tui_running=0;
    pthread_join(ptid,NULL);
    endwin();
    return 0;
}
