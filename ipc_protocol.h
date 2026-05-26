#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>

#define IPC_SOCK_PATH   "/tmp/minici.sock"
#define IPC_MAX_PAYLOAD 8192

/* ── TUI → Server 명령 타입 ── */
typedef enum {
    CMD_STATUS       = 1,   /* 서버 상태 조회 (기존)              */
    CMD_PAUSE        = 2,   /* 채점 일시정지 (기존)               */
    CMD_RESUME       = 3,   /* 채점 재개 (기존)                   */
    CMD_SET_TIMEOUT  = 4,   /* 타임아웃 변경 (기존)               */

    CMD_SET_HW       = 10,  /* [기능 2] 주차 전환  "week_2"       */
    CMD_SET_STUDENTS = 11,  /* [기능 3] 수강생 수 등록            */
    CMD_GET_TRACKER  = 12,  /* [기능 3] 현황 배열 요청            */
    CMD_EXPORT_CSV   = 13,  /* [기능 4] CSV 강제 flush            */
    CMD_GET_CSV_STAT = 14,  /* [기능 4] CSV 저장 상태 조회        */
    CMD_TOGGLE_DIST  = 15,  /* 파일 배포 On/Off                   */
} IpcCommand;

/* ── TUI → Server 요청 패킷 ── */
typedef struct __attribute__((packed)) {
    uint8_t cmd;                     /* IpcCommand                        */
    char    payload[IPC_MAX_PAYLOAD];/* 명령별 인자                       */
} IpcRequest;

/* ── Server → TUI 응답 상태 ── */
typedef enum {
    IPC_OK    = 0,
    IPC_ERROR = 1,
} IpcStatus;

/* ── Server → TUI 응답 패킷 ── */
typedef struct __attribute__((packed)) {
    uint8_t status;                  /* IpcStatus                         */
    char    data[IPC_MAX_PAYLOAD];   /* 응답 데이터 (파이프 구분 문자열) */
} IpcResponse;

#endif /* IPC_PROTOCOL_H */
