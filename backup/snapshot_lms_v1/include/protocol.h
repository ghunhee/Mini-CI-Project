#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAGIC_NUMBER   0x4D494E49   /* 'MINI' — 패킷 식별자            */
#define PROTO_VERSION  2            /* 헤더 버전 (하위 호환 검증용)     */
#define MAX_STUDENT_ID 16           /* 학번 최대 길이 (null 포함)       */
#define MAX_FILENAME   128          /* 파일명 최대 길이                 */
#define MAX_WEEK_NAME  32           /* 과제 주차명 최대 길이            */
#define PORT           8080

/* ── 클라이언트 → 서버 패킷 타입 ── */
typedef enum {
    PKT_SUBMIT    = 1,  /* 소스코드 제출 */
    PKT_HEARTBEAT = 2,  /* 연결 유지 (추후 확장) */
} ClientPacketType;

/* ── 서버 → 클라이언트 응답 타입 ── */
typedef enum {
    RES_RESULT = 1,  /* 채점 결과 */
    RES_ACK    = 2,  /* 접수 확인 */
    RES_ERROR  = 3,  /* 에러 메시지 */
} ServerPacketType;

/* ── 채점 결과 코드 ── */
typedef enum {
    GRADE_AC  = 0,  /* Accepted          */
    GRADE_WA  = 1,  /* Wrong Answer      */
    GRADE_TLE = 2,  /* Time Limit Exceed */
    GRADE_RE  = 3,  /* Runtime Error     */
    GRADE_CE  = 4,  /* Compile Error     */
} GradeResult;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;
    uint16_t header_size;
    char     student_id[MAX_STUDENT_ID];
    char     filename[MAX_FILENAME];
    uint32_t file_size;
    uint32_t checksum;
} SubmitHeader;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;
    uint16_t header_size;
    char     student_id[MAX_STUDENT_ID];
    char     filename[MAX_FILENAME];
    uint8_t  result;
    uint32_t exec_time_ms;
    uint32_t memory_kb;
    uint16_t submitted_count;
    uint16_t total_students;
    char     week_name[MAX_WEEK_NAME];
    char     message[128];
} ResultHeader;

#endif /* PROTOCOL_H */
