#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#define MAX_LINE_LEN 2048
#define MAX_IP_LEN   64
#define MAX_URL_LEN  256
#define MAX_FILES    16
#define SHM_NAME     "/logpulse_shm"

typedef enum {
    MODE_LOTE,
    MODE_VIVO
} RunMode;

typedef struct {
    char ip[MAX_IP_LEN];
    char timestamp[64];
    char method[16];
    char url[MAX_URL_LEN]; // Sin cadena de consulta para el ranking
    int status_code;
    uint64_t bytes;
    double response_time;  // -1.0 si no viene el tiempo final
    bool valid;            // false si la línea está mal formada
} LogEntry;

typedef enum {
    MSG_STARTED,
    MSG_FINISHED,
    MSG_ERROR,
    MSG_ROTATED,
    MSG_DIED
} PipeMsgType;

typedef struct {
    int worker_index;
    pid_t pid;
    PipeMsgType type;
    char detail[128];
} PipeMessage;

#endif
