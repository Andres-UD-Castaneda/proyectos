#ifndef WORKER_H
#define WORKER_H

#include "common.h"
#include "metrics.h"

typedef struct {
    int worker_index;
    const char *filepath;
    RunMode mode;
    int num_parsers;
    int status_pipe_fd;
    SharedMemoryData *shm;
} WorkerConfig;

int run_worker_process(const WorkerConfig *cfg);

#endif
