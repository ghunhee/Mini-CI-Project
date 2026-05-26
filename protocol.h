// protocol.h — client.c / server.c 공통 사용
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAGIC_NUMBER     0x4D494E49   // 'MINI'
#define PROTO_VERSION    2
#define MAX_STUDENT_ID   16
#define MAX_FILENAME     128
#define MAX_WEEK_NAME    32

// ── 클라이언트 → 서버 패킷 타입 ──────────────────────────────
typedef enum {
    PKT_SUBMIT   = 1,   // 코드 제출 (기존 기능)
    PKT_HEARTBEAT = 2,  // 연결 유지 (추후 확장)
} ClientPacketType;

// ── 서버 → 클라이언트 응답 타입 ──────────────────────────────
typedef enum {
    RES_RESULT  = 1,   // 채점 결과 반환
    RES_ACK     = 2,   // 제출 접수 확인
    RES_ERROR   = 3,   // 에러 메시지
} ServerPacketType;

// ── 채점 결과 코드 ─────────────────────────────────────────────
typedef enum {
    GRADE_AC  = 0,
    GRADE_WA  = 1,
    GRADE_TLE = 2,
    GRADE_RE  = 3,
    GRADE_CE  = 4,   // Compile Error (신규)
} GradeResult;

/* ============================================================
 *  [핵심] 클라이언트 → 서버 제출 헤더 (기능 1, 2 반영)
 *  고정 크기 헤더 뒤에 file_size 바이트의 소스코드 페이로드가 붙음
 * ============================================================ */
typedef struct __attribute__((packed)) {
    uint32_t magic;                    // 0x4D494E49
    uint8_t  version;                  // PROTO_VERSION
    uint8_t  pkt_type;                 // ClientPacketType
    uint16_t header_size;              // sizeof(SubmitHeader) — 수신 측 검증용

    // ── 기능 1: 학번 인증 ──────────────────────────────────
    char     student_id[MAX_STUDENT_ID]; // "20240123\0..."

    // ── 기존 파일 정보 ──────────────────────────────────────
    char     filename[MAX_FILENAME];   // "hw1_calculator.c"
    uint32_t file_size;                // 소스코드 바이트 수

    // ── 무결성 검증 ─────────────────────────────────────────
    uint32_t checksum;                 // CRC32 of payload
} SubmitHeader;

/* ============================================================
 *  서버 → 클라이언트 채점 결과 패킷 (기능 3 실시간 현황 포함)
 * ============================================================ */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;                 // ServerPacketType
    uint16_t header_size;

    char     student_id[MAX_STUDENT_ID];
    char     filename[MAX_FILENAME];
    uint8_t  result;                   // GradeResult

    uint32_t exec_time_ms;             // 실행 시간 (밀리초)
    uint32_t memory_kb;                // 메모리 사용량 (KB)

    // ── 기능 3: 실시간 현황 정보 (서버가 덤으로 내려줌) ────
    uint16_t submitted_count;          // 현재까지 제출자 수
    uint16_t total_students;           // 전체 수강생 수

    char     week_name[MAX_WEEK_NAME]; // 현재 과제 주차 "week_2"
    char     message[128];             // 에러 메시지 등 부가 설명
} ResultHeader;

#endif // PROTOCOL_H
