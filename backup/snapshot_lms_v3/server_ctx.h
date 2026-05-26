#ifndef SERVER_CTX_H
#define SERVER_CTX_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "include/protocol.h"
#include "ipc_protocol.h"

#define MAX_STUDENTS    512           /* 동시 수강생 최대 수               */
#define SUBMISSIONS_DIR "submissions" /* 제출 파일 루트 디렉토리           */
#define CSV_PATH        "results.csv" /* 성적 CSV 파일 경로                */

/* ── [기능 3] 학생 1명의 제출 추적 레코드 ── */
typedef struct {
    char     student_id[MAX_STUDENT_ID]; /* 학번                          */
    uint8_t  submitted;                  /* 0=미제출, 1=AC 달성           */
    uint8_t  best_result;                /* GradeResult 중 최고 결과      */
    uint32_t submit_count;               /* 총 제출 횟수 (재채점 포함)    */
    time_t   last_submit_ts;             /* 마지막 제출 시각              */
} StudentRecord;

/* ── [기능 4] CSV 쓰기 상태 ── */
typedef struct {
    uint32_t total_records;  /* 누적 기록 수                              */
    time_t   last_write_ts;  /* 마지막 기록 시각                          */
    int      csv_fd;         /* 열린 파일 디스크립터 (append 모드)        */
} CsvState;

/*
 * 전역 서버 컨텍스트 — server.c 에서 단 하나의 인스턴스(g_ctx)로 사용.
 *
 *   hw_lock      — [기능 2] current_hw / ans_path / submit_dir 보호
 *   tracker_lock — [기능 3] students 배열 보호
 *   csv_lock     — [기능 4] csv 구조체 보호
 *   state_lock   — paused / timeout_ms 보호
 */
typedef struct {
    /* [기능 2] 현재 과제 주차 */
    char             current_hw[MAX_WEEK_NAME]; /* "week_2"              */
    char             ans_path[256];             /* "week_2_ans.txt"      */
    char             submit_dir[256];           /* "submissions/week_2/" */
    pthread_rwlock_t hw_lock;                   /* RW 락 — 무중단 전환  */

    /* [기능 3] 수강생 현황 배열 */
    StudentRecord    students[MAX_STUDENTS];
    uint16_t         total_students;            /* set-students 로 설정  */
    uint16_t         submitted_count;           /* AC 달성 학생 수       */
    pthread_mutex_t  tracker_lock;

    /* [기능 4] CSV 상태 */
    CsvState         csv;
    pthread_mutex_t  csv_lock;

    /* 기존: 서버 동작 상태 */
    volatile int     paused;
    volatile int     dist_paused;
    uint32_t         timeout_ms;
    pthread_mutex_t  state_lock;
} ServerContext;

/* 전역 인스턴스 — server_ctx.c 에서 정의, 나머지 파일에서 extern */
extern ServerContext g_ctx;

/* 함수 프로토타입 */
void ctx_init(ServerContext *ctx);
void ctx_set_hw(ServerContext *ctx, const char *week_name);
void ctx_record_submission(ServerContext *ctx,
                           const char *student_id,
                           GradeResult result);
void ctx_write_csv(ServerContext *ctx,
                   const char *student_id,
                   GradeResult result,
                   uint32_t exec_ms,
                   uint32_t mem_kb);

#endif /* SERVER_CTX_H */
