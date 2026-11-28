#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "metrics.h"

void metrics_shm_init(SharedMemoryData *shm, int num_workers) {
    if (!shm) {
        return;
    }
    memset(shm, 0, sizeof(*shm));
    if (sem_init(&shm->mutex, 1, 1) != 0) {
        perror("sem_init en metrics_shm_init");
    }
    if (num_workers < 0) {
        num_workers = 0;
    }
    if (num_workers > MAX_FILES) {
        num_workers = MAX_FILES;
    }
    shm->num_workers = num_workers;
}

void metrics_shm_destroy(SharedMemoryData *shm) {
    if (!shm) {
        return;
    }
    if (sem_destroy(&shm->mutex) != 0) {
        perror("sem_destroy en metrics_shm_destroy");
    }
}

void metrics_worker_reset(WorkerMetrics *wm, pid_t pid, const char *filename, bool active) {
    if (!wm) {
        return;
    }
    memset(wm, 0, sizeof(*wm));
    wm->pid = pid;
    wm->active = active;
    wm->crashed = false;
    if (filename) {
        snprintf(wm->filename, sizeof(wm->filename), "%s", filename);
    }
}

static void upsert_stat_item(StatItem *arr, int *count, const char *key, double resp_time) {
    if (!arr || !count || !key || key[0] == '\0') {
        return;
    }
    double add_time = (resp_time >= 0.0) ? resp_time : 0.0;

    for (int i = 0; i < *count; i++) {
        if (strcmp(arr[i].key, key) == 0) {
            arr[i].count++;
            arr[i].total_time += add_time;
            return;
        }
    }

    if (*count < MAX_TRACKED_ITEMS) {
        int idx = (*count)++;
        snprintf(arr[idx].key, sizeof(arr[idx].key), "%s", key);
        arr[idx].count = 1;
        arr[idx].total_time = add_time;
    }
}

void metrics_record_entry(WorkerMetrics *wm, const LogEntry *entry) {
    if (!wm || !entry) {
        return;
    }

    if (!entry->valid) {
        wm->malformed_lines++;
        return;
    }

    wm->total_requests++;
    wm->bytes_transferred += entry->bytes;

    if (entry->status_code >= 200 && entry->status_code < 300) {
        wm->status_2xx++;
    } else if (entry->status_code >= 300 && entry->status_code < 400) {
        wm->status_3xx++;
    } else if (entry->status_code >= 400 && entry->status_code < 500) {
        wm->status_4xx++;
    } else if (entry->status_code >= 500 && entry->status_code < 600) {
        wm->status_5xx++;
    }

    upsert_stat_item(wm->ips, &wm->ip_count, entry->ip, entry->response_time);
    upsert_stat_item(wm->urls, &wm->url_count, entry->url, entry->response_time);
}

static int cmp_stat_desc(const void *a, const void *b) {
    const StatItem *sa = (const StatItem *)a;
    const StatItem *sb = (const StatItem *)b;
    if (sb->count > sa->count) {
        return 1;
    }
    if (sb->count < sa->count) {
        return -1;
    }
    return strcmp(sa->key, sb->key);
}

int metrics_top_items(const StatItem *items, int count, StatItem *out_top, int max_top) {
    if (!items || !out_top || count <= 0 || max_top <= 0) {
        return 0;
    }

    int n = (count < MAX_TRACKED_ITEMS) ? count : MAX_TRACKED_ITEMS;
    StatItem *copy = malloc((size_t)n * sizeof(StatItem));
    if (!copy) {
        return 0;
    }

    memcpy(copy, items, (size_t)n * sizeof(StatItem));
    qsort(copy, (size_t)n, sizeof(StatItem), cmp_stat_desc);

    int out_n = (n < max_top) ? n : max_top;
    memcpy(out_top, copy, (size_t)out_n * sizeof(StatItem));
    free(copy);
    return out_n;
}

int metrics_export_csv(const SharedMemoryData *shm, const char *csv_path) {
    if (!shm || !csv_path || csv_path[0] == '\0') {
        return -1;
    }

    SharedMemoryData *mutable_shm = (SharedMemoryData *)shm;
    while (sem_wait(&mutable_shm->mutex) != 0) {
        if (errno != EINTR) {
            perror("sem_wait en metrics_export_csv");
            return -1;
        }
    }

    FILE *fp = fopen(csv_path, "w");
    if (!fp) {
        perror("fopen en metrics_export_csv");
        sem_post(&mutable_shm->mutex);
        return -1;
    }

    fprintf(fp,
            "worker_idx,pid,active,crashed,filename,total_requests,malformed_lines,"
            "bytes_transferred,status_2xx,status_3xx,status_4xx,status_5xx,elapsed_seconds\n");

    for (int i = 0; i < shm->num_workers; i++) {
        const WorkerMetrics *wm = &shm->workers[i];
        fprintf(fp,
                "%d,%d,%d,%d,\"%s\",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f\n",
                i,
                (int)wm->pid,
                wm->active ? 1 : 0,
                wm->crashed ? 1 : 0,
                wm->filename,
                (unsigned long long)wm->total_requests,
                (unsigned long long)wm->malformed_lines,
                (unsigned long long)wm->bytes_transferred,
                (unsigned long long)wm->status_2xx,
                (unsigned long long)wm->status_3xx,
                (unsigned long long)wm->status_4xx,
                (unsigned long long)wm->status_5xx,
                wm->elapsed_seconds);
    }

    fclose(fp);
    sem_post(&mutable_shm->mutex);
    return 0;
}
