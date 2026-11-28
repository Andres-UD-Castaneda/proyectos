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

void metrics_shm_init(SharedMemoryData *shm, int num_workers);
void metrics_shm_destroy(SharedMemoryData *shm);
void metrics_worker_reset(WorkerMetrics *wm, pid_t pid, const char *filename, bool active);
void metrics_record_entry(WorkerMetrics *wm, const LogEntry *entry);
int metrics_top_items(const StatItem *items, int count, StatItem *out_top, int max_top);
int metrics_export_csv(const SharedMemoryData *shm, const char *csv_path);
