#ifndef BUFFER_H
#define BUFFER_H

#include <stdbool.h>
#include <pthread.h>
#include "common.h"

#define BUFFER_CAPACITY 1024

typedef struct {
    char lines[BUFFER_CAPACITY][MAX_LINE_LEN];
    int head;
    int tail;
    int count;
    bool closed; // Indica que el lector terminó (píldora venenosa / EOF)
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} BoundedBuffer;

int buffer_init(BoundedBuffer *buf);
void buffer_destroy(BoundedBuffer *buf);
bool buffer_push(BoundedBuffer *buf, const char *line);
bool buffer_pop(BoundedBuffer *buf, char *out_line);
void buffer_close(BoundedBuffer *buf);

#endif
