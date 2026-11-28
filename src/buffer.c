#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include "buffer.h"

int buffer_init(BoundedBuffer *buf) {
    if (!buf) {
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    buf->closed = false;

    if (pthread_mutex_init(&buf->mutex, NULL) != 0) {
        perror("pthread_mutex_init en buffer_init");
        return -1;
    }

    if (pthread_cond_init(&buf->not_empty, NULL) != 0) {
        perror("pthread_cond_init(not_empty) en buffer_init");
        pthread_mutex_destroy(&buf->mutex);
        return -1;
    }

    if (pthread_cond_init(&buf->not_full, NULL) != 0) {
        perror("pthread_cond_init(not_full) en buffer_init");
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return -1;
    }

    return 0;
}

void buffer_destroy(BoundedBuffer *buf) {
    if (!buf) {
        return;
    }

    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

bool buffer_push(BoundedBuffer *buf, const char *line) {
    if (!buf || !line) {
        return false;
    }

    pthread_mutex_lock(&buf->mutex);

    while (buf->count == BUFFER_CAPACITY && !buf->closed) {
        pthread_cond_wait(&buf->not_full, &buf->mutex);
    }

    if (buf->closed) {
        pthread_mutex_unlock(&buf->mutex);
        return false;
    }

    snprintf(buf->lines[buf->tail], MAX_LINE_LEN, "%s", line);
    buf->tail = (buf->tail + 1) % BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return true;
}

bool buffer_pop(BoundedBuffer *buf, char *out_line) {
    if (!buf || !out_line) {
        return false;
    }

    pthread_mutex_lock(&buf->mutex);

    while (buf->count == 0 && !buf->closed) {
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    }

    if (buf->count == 0 && buf->closed) {
        pthread_mutex_unlock(&buf->mutex);
        return false;
    }

    snprintf(out_line, MAX_LINE_LEN, "%s", buf->lines[buf->head]);
    buf->head = (buf->head + 1) % BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return true;
}

void buffer_close(BoundedBuffer *buf) {
    if (!buf) {
        return;
    }

    pthread_mutex_lock(&buf->mutex);
    buf->closed = true;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}
