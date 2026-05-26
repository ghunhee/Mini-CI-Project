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

#include "ipc_protocol.h"
#include "include/protocol.h"

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
static char g_deadline[64]  = "미설정";
static char g_testsh[256]   = "";

static WINDOW *w_hdr  = NULL;
static WINDOW *w_menu = NULL;
static WINDOW *w_info = NULL;
static WINDOW *w_log  = NULL;

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

static void draw_hdr(void) {
    int cols = getmaxx(w_hdr);
    int pct  = g_snap.total>0 ? (int)(g_snap.submitted*100/g_snap.total) : 0;
    werase(w_hdr);
    wattron(w_hdr, COLOR_PAIR(6)|A_BOLD);
    mvwhline(w_hdr,0,0,' ',cols);
    mvwprintw(w_hdr,0,1,
        " Mini CI 관리자  |  주차: %-10s  |  제출: %u/%u (%d%%)  |  기한: %-20s  |  CSV: %u건",
        g_snap.week, g_snap.submitted, g_snap.total, pct, g_deadline, g_snap.csv_records);
    wattroff(w_hdr, COLOR_PAIR(6)|A_BOLD);
    wrefresh(w_hdr);
}

static void draw_info(void) {
    int rows = getmaxy(w_info);
    int cols = getmaxx(w_info)-4;
    werase(w_info); box(w_info,0,0);
    wattron(w_info,A_BOLD);
    mvwprintw(w_info,1,2,"[ 실시간 제출 현황 ]");
    wattroff(w_info,A_BOLD);
    int pct  = g_snap.total>0 ? (int)(g_snap.submitted*100/g_snap.total) : 0;
    int fill = cols>0 ? (cols*pct)/100 : 0;
    mvwprintw(w_info,2,2,"[");
    wattron(w_info,COLOR_PAIR(2));
    for(int i=0;i<fill;i++) waddch(w_info,ACS_CKBOARD);
    wattroff(w_info,COLOR_PAIR(2));
    wattron(w_info,COLOR_PAIR(3));
    for(int i=fill;i<cols;i++) waddch(w_info,'-');
    wattroff(w_info,COLOR_PAIR(3));
    wprintw(w_info,"] %u/%u  %d%%", g_snap.submitted, g_snap.total, pct);
    if (g_testsh[0]) mvwprintw(w_info,3,2,"test.sh: %s", g_testsh);
    static const char *gn[]={"AC","WA","TLE","RE","CE"};
    int maxr = rows-6;
    for(int i=0;i<g_snap.entry_count && i<maxr;i++){
        int pair=(g_snap.entries[i].result==0)?2:4;
        uint8_t r=g_snap.entries[i].result<5?g_snap.entries[i].result:4;
        wattron(w_info,COLOR_PAIR(pair));
        mvwprintw(w_info,5+i,2,"%-14s [%-3s]",g_snap.entries[i].id,gn[r]);
        wattroff(w_info,COLOR_PAIR(pair));
    }
    wrefresh(w_info);
}

static void log_msg(int pair, const char *msg) {
    wattron(w_log,COLOR_PAIR(pair));
    wprintw(w_log,"%s\n",msg);
    wattroff(w_log,COLOR_PAIR(pair));
    wrefresh(w_log);
}

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
            pthread_mutex_lock(&snap_mu); g_snap=t; pthread_mutex_unlock(&snap_mu);
            draw_hdr(); draw_info();
        }
        sleep(1);
    }
    return NULL;
}

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
    mvwgetnstr(pop,5,4,out,maxlen-1);
    noecho(); curs_set(0);
    delwin(pop); touchwin(stdscr); refresh();
    draw_hdr(); draw_info();
    return strlen(out)>0 ? 0 : -1;
}

static void distribute_testsh(void) {
    if(g_testsh[0]=='\0') { log_msg(4,"[오류] test.sh 경로가 설정되지 않았습니다."); return; }
    char dst[512];
    snprintf(dst,sizeof(dst),"submissions/%s/test.sh",g_snap.week);
    char cmd[600];
    snprintf(cmd,sizeof(cmd),"mkdir -p submissions/%s && cp \"%s\" \"%s\"",
             g_snap.week, g_testsh, dst);
    int ret = system(cmd);
    char msg[300];
    if(ret==0) snprintf(msg,sizeof(msg),"[배포 완료] %s -> %s", g_testsh, dst);
    else       snprintf(msg,sizeof(msg),"[배포 실패] 파일을 확인하세요: %s", g_testsh);
    log_msg(ret==0?2:4, msg);
}

typedef struct {
    const char *icon;
    const char *label;
    const char *desc;
    int         action;
    IpcCommand  cmd;
    int         needs_input;
} MenuItem;

#define ACT_IPC    0
#define ACT_DL     1
#define ACT_TSSET  2
#define ACT_TSDIST 3

static MenuItem MENU[] = {
    {"[현황]", "서버 상태 조회",      "현재 워커/큐 상태를 확인합니다",          ACT_IPC,   CMD_STATUS,       0},
    {"[주차]", "과제 주차 변경",      "채점 기준 주차를 바꿉니다 (예: week_3)",  ACT_IPC,   CMD_SET_HW,       1},
    {"[인원]", "수강생 수 설정",      "전체 수강생 수를 입력합니다 (예: 30)",    ACT_IPC,   CMD_SET_STUDENTS, 1},
    {"[기한]", "제출 기한 설정",      "마감 일시를 설정합니다 (예: 5/31 23:59)", ACT_DL,    0,                1},
    {"[배포]", "test.sh 경로 설정",   "배포할 test.sh 파일 경로를 지정합니다",   ACT_TSSET, 0,                1},
    {"[배포]", "test.sh 학생 배포",   "지정된 test.sh를 제출 폴더에 복사합니다", ACT_TSDIST,0,               0},
    {"[정지]", "채점 일시정지",       "새 제출을 잠시 받지 않습니다",            ACT_IPC,   CMD_PAUSE,        0},
    {"[재개]", "채점 재개",           "제출 접수를 다시 시작합니다",             ACT_IPC,   CMD_RESUME,       0},
    {"[CSV]",  "성적표 즉시 저장",    "results.csv 를 지금 flush 합니다",        ACT_IPC,   CMD_EXPORT_CSV,   0},
    {"[CSV]",  "성적표 저장 현황",    "CSV 누적 레코드 수와 마지막 저장 시각",   ACT_IPC,   CMD_GET_CSV_STAT, 0},
};
static const int MENU_CNT = (int)(sizeof(MENU)/sizeof(MENU[0]));

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

    w_hdr  = newwin(top_h, C,            0,           0);
    w_menu = newwin(mid_h, menu_w,       top_h,       0);
    w_info = newwin(mid_h, info_w,       top_h,       menu_w);
    w_log  = newwin(bot_h, C,            top_h+mid_h, 0);
    scrollok(w_log,TRUE); keypad(w_menu,TRUE);

    IpcResponse ir; memset(&ir,0,sizeof(ir));
    if(ipc_send(CMD_GET_TRACKER,NULL,&ir)==0) parse_snap(ir.data,&g_snap);
    draw_hdr(); draw_info();

    pthread_t ptid; pthread_create(&ptid,NULL,poll_fn,NULL);

    int sel=0;
    while(1) {
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

        int ch=wgetch(w_menu);
        if(ch=='q'||ch==KEY_F(10)) break;
        if(ch==KEY_UP)   { sel=(sel-1+MENU_CNT)%MENU_CNT; continue; }
        if(ch==KEY_DOWN) { sel=(sel+1)%MENU_CNT; continue; }

        if(ch=='\n'||ch==KEY_ENTER) {
            MenuItem *m=&MENU[sel];
            char input[IPC_MAX_PAYLOAD]={0};
            char msg[IPC_MAX_PAYLOAD+64]={0};

            if(m->action==ACT_DL) {
                if(popup_input("제출 기한 설정","2026-05-31 23:59",input,sizeof(input))==0)
                    strncpy(g_deadline,input,sizeof(g_deadline)-1);
                draw_hdr(); continue;
            }
            if(m->action==ACT_TSSET) {
                if(popup_input("test.sh 파일 경로","~/hw/test.sh",input,sizeof(input))==0)
                    strncpy(g_testsh,input,sizeof(g_testsh)-1);
                snprintf(msg,sizeof(msg),"[설정] test.sh 경로: %s",g_testsh);
                log_msg(2,msg); continue;
            }
            if(m->action==ACT_TSDIST) { distribute_testsh(); continue; }

            if(m->needs_input) {
                const char *hint  = (m->cmd==CMD_SET_HW)?"week_3":"30";
                const char *title = (m->cmd==CMD_SET_HW)?"과제 주차 입력 (예: week_3)":"수강생 수 입력 (예: 30)";
                if(popup_input(title,hint,input,sizeof(input))<0) continue;
            }
            IpcResponse resp; memset(&resp,0,sizeof(resp));
            int ret=ipc_send(m->cmd, m->needs_input?input:NULL, &resp);
            if(ret<0) {
                log_msg(4,"[오류] 서버에 연결할 수 없습니다. 서버가 실행 중인지 확인하세요.");
            } else if(resp.status==IPC_OK) {
                snprintf(msg,sizeof(msg),"[완료] %s",resp.data);
                log_msg(2,msg);
                if(m->cmd==CMD_SET_HW && m->needs_input) {
                    pthread_mutex_lock(&snap_mu);
                    strncpy(g_snap.week,input,MAX_WEEK_NAME-1);
                    pthread_mutex_unlock(&snap_mu);
                    draw_hdr();
                }
            } else {
                snprintf(msg,sizeof(msg),"[실패] %s",resp.data);
                log_msg(4,msg);
            }
        }
    }

    tui_running=0;
    pthread_join(ptid,NULL);
    endwin();
    return 0;
}
