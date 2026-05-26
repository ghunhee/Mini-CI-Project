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
    PKT_GET_FILES = 3,   /* 배포 파일 목록+내용 요청 */
} ClientPacketType;

/* ── 서버 → 클라이언트 응답 타입 ── */
typedef enum {
    RES_RESULT = 1,  /* 채점 결과 */
    RES_ACK    = 2,  /* 접수 확인 */
    RES_ERROR  = 3,  /* 에러 메시지 */
    RES_FILE_META  = 10, // 파일 1개의 메타데이터 (이름·크기)
    RES_FILE_CHUNK = 11, // 파일 데이터 청크
    RES_FILE_END   = 12, // 해당 파일 전송 완료
    RES_FILE_DONE  = 13, // 전체 배포 파일 전송 완료
    RES_FILE_ERROR = 14, // 에러 (파일 없음 등)
} ServerPacketType;

/* ── 채점 결과 코드 ── */
typedef enum {
    GRADE_AC  = 0,  /* Accepted          */
    GRADE_WA  = 1,  /* Wrong Answer      */
    GRADE_TLE = 2,  /* Time Limit Exceed */
    GRADE_RE  = 3,  /* Runtime Error     */
    GRADE_CE  = 4,  /* Compile Error     */
} GradeResult;

/*
 * [기능 1, 2] 클라이언트 → 서버 제출 헤더
 * 고정 크기 헤더 뒤에 file_size 바이트의 소스코드 페이로드가 이어짐.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;                      /* MAGIC_NUMBER                  */
    uint8_t  version;                    /* PROTO_VERSION                 */
    uint8_t  pkt_type;                   /* ClientPacketType              */
    uint16_t header_size;                /* sizeof(SubmitHeader) — 검증용 */

    char     student_id[MAX_STUDENT_ID]; /* [기능 1] 학번 "20240123"      */
    char     filename[MAX_FILENAME];     /* "hw1_calculator.c"            */
    uint32_t file_size;                  /* 소스코드 바이트 수            */
    uint32_t checksum;                   /* CRC32 of payload              */
} SubmitHeader;

/*
 * [기능 3] 서버 → 클라이언트 채점 결과 헤더
 * submitted_count / total_students 로 실시간 현황을 클라이언트에 내려줌.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;                   /* ServerPacketType              */
    uint16_t header_size;

    char     student_id[MAX_STUDENT_ID];
    char     filename[MAX_FILENAME];
    uint8_t  result;                     /* GradeResult                   */

    uint32_t exec_time_ms;               /* 실행 시간 (ms)                */
    uint32_t memory_kb;                  /* 메모리 사용량 (KB)            */

    uint16_t submitted_count;            /* [기능 3] 현재까지 완료 인원   */
    uint16_t total_students;             /* [기능 3] 전체 수강생 수       */

    char     week_name[MAX_WEEK_NAME];   /* [기능 2] 현재 과제 주차       */
    char     message[128];               /* 부가 설명 / 에러 메시지       */
} ResultHeader;

#ifndef PULL_PROTOCOL_DEFINED
#define PULL_PROTOCOL_DEFINED

#include <stdint.h>

/* 파일 청크 크기: 64 KB. */
#define FILE_CHUNK_SIZE  (64 * 1024)

/* 배포 파일이 저장되는 서버 루트 디렉토리. */
#define DIST_ROOT  "dist"

/* 한 번에 배포할 수 있는 파일 최대 수 */
#define MAX_DIST_FILES  64

/*
 * GetFilesHeader — 클라이언트 → 서버 (PKT_GET_FILES)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;                      /* MAGIC_NUMBER                  */
    uint8_t  version;                    /* PROTO_VERSION                 */
    uint8_t  pkt_type;                   /* PKT_GET_FILES = 3             */
    uint16_t header_size;                /* sizeof(GetFilesHeader)        */
    char     student_id[MAX_STUDENT_ID]; /* 로그용 (없으면 "anonymous")  */
} GetFilesHeader;

/*
 * FileMetaHeader — 서버 → 클라이언트 (RES_FILE_META)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;                   /* RES_FILE_META = 10            */
    uint16_t header_size;

    char     filename[MAX_FILENAME];     /* 파일명 (경로 없이 베이스만)  */
    uint32_t file_size;                  /* 전체 파일 바이트 수          */
    uint8_t  is_executable;             /* 1이면 클라이언트가 chmod +x  */
    uint8_t  file_index;                 /* 현재 파일 번호 (1-based)     */
    uint8_t  total_files;               /* 전체 배포 파일 수            */
    uint8_t  _pad;
} FileMetaHeader;

/*
 * FileChunkHeader — 서버 → 클라이언트 (RES_FILE_CHUNK)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;                   /* RES_FILE_CHUNK = 11           */
    uint16_t header_size;

    uint32_t chunk_size;                 /* 이 청크의 바이트 수          */
    uint32_t offset;                     /* 파일 내 오프셋               */
} FileChunkHeader;

/*
 * FileEndHeader — 서버 → 클라이언트 (RES_FILE_END / RES_FILE_DONE)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;                   /* RES_FILE_END=12 / DONE=13    */
    uint16_t header_size;

    uint32_t checksum;                   /* CRC32 of entire file         */
    uint8_t  total_files;               /* RES_FILE_DONE 시 전체 파일 수 */
    uint8_t  _pad[3];
} FileEndHeader;

/*
 * FileErrorHeader — 서버 → 클라이언트 (RES_FILE_ERROR)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;                   /* RES_FILE_ERROR = 14          */
    uint16_t header_size;

    char     message[128];               /* 에러 메시지                  */
} FileErrorHeader;

#endif /* PULL_PROTOCOL_DEFINED */

#endif /* PROTOCOL_H */
