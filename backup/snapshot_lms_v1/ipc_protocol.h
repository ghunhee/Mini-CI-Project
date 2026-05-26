#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>

#define IPC_SOCK_PATH   "/tmp/minici.sock"
#define IPC_MAX_PAYLOAD 256

typedef enum {
    CMD_STATUS       = 1,
    CMD_PAUSE        = 2,
    CMD_RESUME       = 3,
    CMD_SET_TIMEOUT  = 4,
    CMD_SET_HW       = 10,
    CMD_SET_STUDENTS = 11,
    CMD_GET_TRACKER  = 12,
    CMD_EXPORT_CSV   = 13,
    CMD_GET_CSV_STAT = 14,
} IpcCommand;

typedef struct __attribute__((packed)) {
    uint8_t cmd;
    char    payload[IPC_MAX_PAYLOAD];
} IpcRequest;

typedef enum {
    IPC_OK    = 0,
    IPC_ERROR = 1,
} IpcStatus;

typedef struct __attribute__((packed)) {
    uint8_t status;
    char    data[IPC_MAX_PAYLOAD];
} IpcResponse;

#endif /* IPC_PROTOCOL_H */
