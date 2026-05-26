#ifndef SERVER_CTX_H
#define SERVER_CTX_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "include/protocol.h"
#include "ipc_protocol.h"

#define MAX_STUDENTS    512
#define SUBMISSIONS_DIR "submissions"
#define CSV_PATH        "results.csv"

typedef struct {
    char     student_id[MAX_STUDENT_ID];
    uint8_t  submitted;
    uint8_t  best_result;
    uint32_t submit_count;
    time_t   last_submit_ts;
} StudentRecord;

typedef struct {
    uint32_t total_records;
    time_t   last_write_ts;
    int      csv_fd;
} CsvState;

typedef struct {
    char             current_hw[MAX_WEEK_NAME];
    char             ans_path[256];
    char             submit_dir[256];
    pthread_rwlock_t hw_lock;

    StudentRecord    students[MAX_STUDENTS];
    uint16_t         total_students;
    uint16_t         submitted_count;
    pthread_mutex_t  tracker_lock;

    CsvState         csv;
    pthread_mutex_t  csv_lock;

    volatile int     paused;
    uint32_t         timeout_ms;
    pthread_mutex_t  state_lock;
} ServerContext;

extern ServerContext g_ctx;

void ctx_init(ServerContext *ctx);
void ctx_set_hw(ServerContext *ctx, const char *week_name);
void ctx_record_submission(ServerContext *ctx, const char *student_id, GradeResult result);
void ctx_write_csv(ServerContext *ctx, const char *student_id, GradeResult result, uint32_t exec_ms, uint32_t mem_kb);

#endif /* SERVER_CTX_H */
