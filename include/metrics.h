#ifndef METRICS_H
#define METRICS_H

#include <semaphore.h>
#include "common.h"

#define TOP_N 10
#define MAX_TRACKED_ITEMS 1024

typedef struct {
    char key[MAX_URL_LEN];
    uint64_t count;
    double total_time;
} StatItem;

typedef struct {
    pid_t pid;
    bool active;
    bool crashed;
    char filename[256];

    uint64_t total_requests;
    uint64_t malformed_lines;
    uint64_t bytes_transferred;
    uint64_t status_2xx;
    uint64_t status_3xx;
    uint64_t status_4xx;
    uint64_t status_5xx;
    double elapsed_seconds;

    StatItem ips[MAX_TRACKED_ITEMS];
    int ip_count;

    StatItem urls[MAX_TRACKED_ITEMS];
    int url_count;
} WorkerMetrics;

typedef struct {
    sem_t mutex; // Inicializar con sem_init(&shm->mutex, 1, 1)
    int num_workers;
    WorkerMetrics workers[MAX_FILES];
} SharedMemoryData;

#endif
