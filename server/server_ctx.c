#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include "server_ctx.h"

/* 전역 인스턴스 정의 */
ServerContext g_ctx;

/* ── 내부 헬퍼: 디렉토리가 없으면 생성 (0755) ── */
static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return 0;
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        perror("[ctx] mkdir");
        return -1;
    }
    return 0;
}

/* ── 내부 헬퍼: GradeResult → 문자열 ── */
static const char *grade_str(GradeResult r) {
    switch (r) {
        case GRADE_AC:  return "AC";
        case GRADE_WA:  return "WA";
        case GRADE_TLE: return "TLE";
        case GRADE_RE:  return "RE";
        case GRADE_CE:  return "CE";
        default:        return "UNKNOWN";
    }
}

/*
 * ctx_init()
 * — 서버 시작 시 main() 에서 단 1회 호출.
 * — 모든 락 초기화 → 기본 과제 주차(week_1) 설정.
 */
void ctx_init(ServerContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    /* ★ 0 초기화로 인한 fd 0 (stdin) 닫힘 방지 */
    ctx->csv.csv_fd = -1;

    /* 락 초기화 */
    pthread_rwlock_init(&ctx->hw_lock,     NULL);
    pthread_mutex_init(&ctx->tracker_lock, NULL);
    pthread_mutex_init(&ctx->csv_lock,     NULL);
    pthread_mutex_init(&ctx->state_lock,   NULL);

    /* 기본 서버 파라미터 */
    ctx->paused          = 0;
    ctx->dist_paused     = 0;
    ctx->timeout_ms      = 5000;
    ctx->total_students  = 0;
    ctx->submitted_count = 0;

    /* [기능 2] 기본 과제 주차 설정 (내부적으로 디렉토리도 생성 & CSV 열기) */
    ctx_set_hw(ctx, "week_1");

    fprintf(stderr, "[ctx] ServerContext 초기화 완료 — %s\n", ctx->current_hw);
}

/*
 * ctx_set_hw()
 *
 * 락 획득 순서 (데드락 방지를 위해 항상 이 순서를 지킨다):
 *   1. hw_lock      (wrlock)
 *   2. tracker_lock (mutex)
 *   3. csv_lock     (mutex)
 */
void ctx_set_hw(ServerContext *ctx, const char *week_name) {
    if (!week_name || strlen(week_name) == 0 ||
        strlen(week_name) >= MAX_WEEK_NAME) {
        fprintf(stderr, "[ctx] set_hw: 잘못된 week_name\n");
        return;
    }

    /* ── 1) hw_lock write: 경로 3개 원자 교체 ── */
    pthread_rwlock_wrlock(&ctx->hw_lock);

    strncpy(ctx->current_hw, week_name, MAX_WEEK_NAME - 1);
    ctx->current_hw[MAX_WEEK_NAME - 1] = '\0';

    snprintf(ctx->ans_path, sizeof(ctx->ans_path),
             "%s_ans.txt", week_name);
    snprintf(ctx->submit_dir, sizeof(ctx->submit_dir),
             "%s/%s", SUBMISSIONS_DIR, week_name);

    pthread_rwlock_unlock(&ctx->hw_lock);

    /* ── 2) 디렉토리 생성 (락 밖, I/O 느릴 수 있음) ── */
    ensure_dir(SUBMISSIONS_DIR);
    ensure_dir(ctx->submit_dir);

    /* ── 3) FIX-4: tracker_lock — students 배열 + 카운터 리셋 ── */
    pthread_mutex_lock(&ctx->tracker_lock);
    memset(ctx->students, 0, sizeof(ctx->students));
    ctx->submitted_count = 0;
    /* total_students 는 유지 (수강생 수는 주차가 바뀌어도 동일) */
    pthread_mutex_unlock(&ctx->tracker_lock);

    /* ── 4) FIX-3: csv_lock — 이전 CSV 닫고 새 주차 CSV 열기 ── */
    pthread_mutex_lock(&ctx->csv_lock);
    {
        /* 기존 파일 닫기 */
        if (ctx->csv.csv_fd >= 0) {
            close(ctx->csv.csv_fd);
            ctx->csv.csv_fd = -1;
        }

        /* 새 주차 CSV 경로: "week_4_results.csv" */
        snprintf(ctx->csv.csv_path, sizeof(ctx->csv.csv_path),
                 "%s_results.csv", week_name);

        int need_header = (access(ctx->csv.csv_path, F_OK) != 0);

        ctx->csv.csv_fd = open(ctx->csv.csv_path,
                               O_WRONLY | O_CREAT | O_APPEND,
                               0644);
        if (ctx->csv.csv_fd < 0) {
            perror("[ctx] open new csv");
        } else {
            if (need_header) {
                const char *hdr =
                    "Timestamp,Student_ID,Assignment_Week,"
                    "Result,ExecTime_ms,Memory_KB\n";
                write(ctx->csv.csv_fd, hdr, strlen(hdr));
            }
            ctx->csv.total_records = 0;
            ctx->csv.last_write_ts = 0;
            fprintf(stderr, "[ctx] CSV 전환: %s\n", ctx->csv.csv_path);
        }
    }
    pthread_mutex_unlock(&ctx->csv_lock);

    fprintf(stderr,
            "[ctx] 주차 전환 완료: %s | 정답지: %s | 제출폴더: %s\n",
            ctx->current_hw, ctx->ans_path, ctx->submit_dir);
}

/*
 * ctx_record_submission()
 * — [기능 3] 채점 완료 시 워커 스레드가 호출.
 * — AC 첫 달성 시에만 submitted_count 증가 (중복 카운트 방지).
 */
uint16_t ctx_record_submission(ServerContext *ctx,
                               const char    *student_id,
                               GradeResult    result) {
    uint16_t current_count = 0;

    pthread_mutex_lock(&ctx->tracker_lock);
    {
        StudentRecord *found = NULL;

        for (int i = 0; i < MAX_STUDENTS; i++) {
            if (ctx->students[i].student_id[0] == '\0') break;
            if (strncmp(ctx->students[i].student_id,
                        student_id, MAX_STUDENT_ID) == 0) {
                found = &ctx->students[i];
                break;
            }
        }

        if (!found) {
            for (int i = 0; i < MAX_STUDENTS; i++) {
                if (ctx->students[i].student_id[0] == '\0') {
                    found = &ctx->students[i];
                    strncpy(found->student_id, student_id, MAX_STUDENT_ID - 1);
                    found->student_id[MAX_STUDENT_ID - 1] = '\0';
                    found->submitted    = 0;
                    found->best_result  = GRADE_WA;
                    found->submit_count = 0;
                    break;
                }
            }
        }

        if (!found) {
            fprintf(stderr, "[ctx] warning: StudentRecord full\n");
        } else {
            if (found->submit_count < UINT32_MAX) {
                found->submit_count++;
            }
            found->last_submit_ts = time(NULL);

            current_count = found->submit_count > UINT16_MAX
                            ? UINT16_MAX
                            : (uint16_t)found->submit_count;

            if (result == GRADE_AC && !found->submitted) {
                found->submitted   = 1;
                found->best_result = GRADE_AC;
                ctx->submitted_count++;
                fprintf(stderr, "[tracker] %s AC reached! (%u / %u)\n",
                        student_id, ctx->submitted_count, ctx->total_students);
            } else if (result != GRADE_AC && found->best_result != GRADE_AC) {
                found->best_result = result;
            }
        }
    }
    pthread_mutex_unlock(&ctx->tracker_lock);

    return current_count;
}

/*
 * ctx_write_csv()
 * — [기능 4] 채점 완료마다 results.csv 에 한 줄 append.
 *
 * 출력 포맷:
 *   Timestamp,Student_ID,Assignment_Week,Result,ExecTime_ms,Memory_KB
 *   2025-05-24T10:32:01,20240123,week_2,AC,342,1024
 */
void ctx_write_csv(ServerContext *ctx,
                   const char    *student_id,
                   GradeResult    result,
                   uint32_t       exec_ms,
                   uint32_t       mem_kb,
                   const char    *week_name) {
    if (ctx->csv.csv_fd < 0) return;

    char ts_buf[32];
    time_t now    = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", tm);

    char line[512];
    int len = snprintf(line, sizeof(line),
                       "%s,%s,%s,%s,%u,%u\n",
                       ts_buf, student_id, week_name,
                       grade_str(result), exec_ms, mem_kb);

    pthread_mutex_lock(&ctx->csv_lock);
    {
        ssize_t written = write(ctx->csv.csv_fd, line, len);
        if (written < 0) {
            perror("[ctx] csv write");
        } else {
            ctx->csv.total_records++;
            ctx->csv.last_write_ts = now;
        }
    }
    pthread_mutex_unlock(&ctx->csv_lock);
}
