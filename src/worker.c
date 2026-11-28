#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "buffer.h"
#include "metrics.h"
#include "parser.h"
#include "worker.h"

#ifndef MAX_WORKERS
#define MAX_WORKERS 16
#endif

static volatile sig_atomic_t worker_stop = 0;

static void handle_worker_signal(int sig) {
    (void)sig;
    worker_stop = 1;
}

typedef struct {
    BoundedBuffer *buf;
    WorkerMetrics *local_acc;
    pthread_mutex_t *acc_mutex;
} ParserThreadArg;

static double elapsed_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double sec = (double)(now.tv_sec - start->tv_sec);
    double nsec = (double)(now.tv_nsec - start->tv_nsec) / 1e9;
    return sec + nsec;
}

static void lock_shm(SharedMemoryData *shm) {
    if (!shm) {
        return;
    }
    while (sem_wait(&shm->mutex) != 0) {
        if (errno != EINTR) {
            break;
        }
    }
}

static void unlock_shm(SharedMemoryData *shm) {
    if (!shm) {
        return;
    }
    sem_post(&shm->mutex);
}

static void send_status_byte(int pipe_fd, char code) {
    if (pipe_fd < 0) {
        return;
    }
    ssize_t w;
    do {
        w = write(pipe_fd, &code, 1);
    } while (w == -1 && errno == EINTR);
}

static void push_chunk_lines(BoundedBuffer *buf, const char *data, size_t len,
                             char *carry, size_t *carry_len) {
    size_t i = 0;
    while (i < len && !worker_stop) {
        const char *nl = memchr(data + i, '\n', len - i);
        if (!nl) {
            size_t rem = len - i;
            if (*carry_len + rem >= MAX_LINE_LEN) {
                size_t space = (MAX_LINE_LEN > *carry_len + 1) ? (MAX_LINE_LEN - 1 - *carry_len) : 0;
                if (space > 0) {
                    memcpy(carry + *carry_len, data + i, space);
                    *carry_len += space;
                }
                carry[*carry_len] = '\0';
                buffer_push(buf, carry);
                *carry_len = 0;
                /* Descartar exceso hasta el próximo '\n' */
                while (i < len && data[i] != '\n') {
                    i++;
                }
            } else {
                memcpy(carry + *carry_len, data + i, rem);
                *carry_len += rem;
                i = len;
            }
        } else {
            size_t seg_len = (size_t)(nl - (data + i));
            char line_out[MAX_LINE_LEN];
            size_t pos = 0;

            if (*carry_len > 0) {
                size_t cpy = (*carry_len < sizeof(line_out) - 1) ? *carry_len : (sizeof(line_out) - 1);
                memcpy(line_out, carry, cpy);
                pos = cpy;
                *carry_len = 0;
            }

            size_t avail = (sizeof(line_out) - 1 > pos) ? (sizeof(line_out) - 1 - pos) : 0;
            size_t d_cpy = (seg_len < avail) ? seg_len : avail;
            if (d_cpy > 0) {
                memcpy(line_out + pos, data + i, d_cpy);
                pos += d_cpy;
            }
            if (pos > 0 && line_out[pos - 1] == '\r') {
                pos--;
            }
            line_out[pos] = '\0';

            if (!buffer_push(buf, line_out)) {
                break;
            }
            i = (size_t)(nl - data) + 1;
        }
    }
}

static int read_range_mmap_or_fd(int fd, off_t offset, size_t length,
                                BoundedBuffer *buf, char *carry, size_t *carry_len) {
    if (length == 0) {
        return 0;
    }

    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    off_t aligned_off = (offset / (off_t)page_size) * (off_t)page_size;
    size_t delta = (size_t)(offset - aligned_off);
    size_t map_len = delta + length;

    void *mapped = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, aligned_off);
    if (mapped != MAP_FAILED) {
        madvise(mapped, map_len, MADV_SEQUENTIAL);
        push_chunk_lines(buf, (const char *)mapped + delta, length, carry, carry_len);
        munmap(mapped, map_len);
        return 0;
    }

    /* Respaldo con read() si mmap no es soportado en el archivo/sistema */
    if (lseek(fd, offset, SEEK_SET) == -1) {
        return -1;
    }
    char io_buf[8192];
    size_t remaining = length;
    while (remaining > 0 && !worker_stop) {
        size_t to_read = (remaining < sizeof(io_buf)) ? remaining : sizeof(io_buf);
        ssize_t r = read(fd, io_buf, to_read);
        if (r <= 0) {
            if (r == -1 && errno == EINTR) {
                continue;
            }
            break;
        }
        push_chunk_lines(buf, io_buf, (size_t)r, carry, carry_len);
        remaining -= (size_t)r;
    }
    return 0;
}

static void *parser_thread_main(void *arg) {
    ParserThreadArg *parg = (ParserThreadArg *)arg;
    char line[MAX_LINE_LEN];
    WorkerMetrics batch_acc;
    memset(&batch_acc, 0, sizeof(batch_acc));

    while (buffer_pop(parg->buf, line)) {
        LogEntry entry;
        parse_log_line(line, &entry);
        metrics_record_entry(&batch_acc, &entry);

        if (batch_acc.total_requests + batch_acc.malformed_lines >= 256) {
            pthread_mutex_lock(parg->acc_mutex);
            parg->local_acc->total_requests += batch_acc.total_requests;
            parg->local_acc->malformed_lines += batch_acc.malformed_lines;
            parg->local_acc->bytes_transferred += batch_acc.bytes_transferred;
            parg->local_acc->status_2xx += batch_acc.status_2xx;
            parg->local_acc->status_3xx += batch_acc.status_3xx;
            parg->local_acc->status_4xx += batch_acc.status_4xx;
            parg->local_acc->status_5xx += batch_acc.status_5xx;
            for (int i = 0; i < batch_acc.ip_count; i++) {
                LogEntry tmp = {0};
                tmp.valid = true;
                snprintf(tmp.ip, sizeof(tmp.ip), "%.*s", (int)(sizeof(tmp.ip) - 1), batch_acc.ips[i].key);
                tmp.response_time = (batch_acc.ips[i].count > 0)
                                        ? (batch_acc.ips[i].total_time / (double)batch_acc.ips[i].count)
                                        : -1.0;
                for (uint64_t k = 0; k < batch_acc.ips[i].count; k++) {
                    metrics_record_entry(parg->local_acc, &tmp);
                }
            }
            pthread_mutex_unlock(parg->acc_mutex);
            memset(&batch_acc, 0, sizeof(batch_acc));
        }
    }

    if (batch_acc.total_requests + batch_acc.malformed_lines > 0) {
        pthread_mutex_lock(parg->acc_mutex);
        parg->local_acc->total_requests += batch_acc.total_requests;
        parg->local_acc->malformed_lines += batch_acc.malformed_lines;
        parg->local_acc->bytes_transferred += batch_acc.bytes_transferred;
        parg->local_acc->status_2xx += batch_acc.status_2xx;
        parg->local_acc->status_3xx += batch_acc.status_3xx;
        parg->local_acc->status_4xx += batch_acc.status_4xx;
        parg->local_acc->status_5xx += batch_acc.status_5xx;
        pthread_mutex_unlock(parg->acc_mutex);
    }

    return NULL;
}

static void sync_metrics_to_shm(SharedMemoryData *shm, int idx, const WorkerMetrics *acc,
                                const struct timespec *start, bool active, bool crashed) {
    if (!shm || idx < 0 || idx >= MAX_FILES) {
        return;
    }
    lock_shm(shm);
    WorkerMetrics *dst = &shm->workers[idx];
    dst->active = active;
    dst->crashed = crashed;
    dst->elapsed_seconds = elapsed_since(start);
    dst->total_requests = acc->total_requests;
    dst->malformed_lines = acc->malformed_lines;
    dst->bytes_transferred = acc->bytes_transferred;
    dst->status_2xx = acc->status_2xx;
    dst->status_3xx = acc->status_3xx;
    dst->status_4xx = acc->status_4xx;
    dst->status_5xx = acc->status_5xx;
    dst->ip_count = acc->ip_count;
    dst->url_count = acc->url_count;
    memcpy(dst->ips, acc->ips, sizeof(dst->ips));
    memcpy(dst->urls, acc->urls, sizeof(dst->urls));
    unlock_shm(shm);
}

int run_worker_process(const WorkerConfig *cfg) {
    if (!cfg || !cfg->filepath || !cfg->shm || cfg->worker_index < 0 || cfg->worker_index >= MAX_FILES) {
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_worker_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    lock_shm(cfg->shm);
    metrics_worker_reset(&cfg->shm->workers[cfg->worker_index], getpid(), cfg->filepath, true);
    unlock_shm(cfg->shm);

    BoundedBuffer buf;
    if (buffer_init(&buf) != 0) {
        lock_shm(cfg->shm);
        cfg->shm->workers[cfg->worker_index].active = false;
        cfg->shm->workers[cfg->worker_index].crashed = true;
        unlock_shm(cfg->shm);
        send_status_byte(cfg->status_pipe_fd, 'E');
        return -1;
    }

    int n_threads = (cfg->num_parsers > 0) ? cfg->num_parsers : 2;
    if (n_threads > 64) {
        n_threads = 64;
    }

    WorkerMetrics local_acc;
    memset(&local_acc, 0, sizeof(local_acc));
    pthread_mutex_t acc_mutex;
    pthread_mutex_init(&acc_mutex, NULL);

    pthread_t *threads = calloc((size_t)n_threads, sizeof(pthread_t));
    ParserThreadArg *targs = calloc((size_t)n_threads, sizeof(ParserThreadArg));
    if (!threads || !targs) {
        free(threads);
        free(targs);
        pthread_mutex_destroy(&acc_mutex);
        buffer_destroy(&buf);
        return -1;
    }

    for (int i = 0; i < n_threads; i++) {
        targs[i].buf = &buf;
        targs[i].local_acc = &local_acc;
        targs[i].acc_mutex = &acc_mutex;
        pthread_create(&threads[i], NULL, parser_thread_main, &targs[i]);
    }

    int fd = open(cfg->filepath, O_RDONLY);
    if (fd < 0) {
        perror("open en worker");
        buffer_close(&buf);
        for (int i = 0; i < n_threads; i++) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
        free(targs);
        pthread_mutex_destroy(&acc_mutex);
        buffer_destroy(&buf);
        sync_metrics_to_shm(cfg->shm, cfg->worker_index, &local_acc, &start_ts, false, true);
        send_status_byte(cfg->status_pipe_fd, 'E');
        return -1;
    }

    struct stat st;
    fstat(fd, &st);
    ino_t current_ino = st.st_ino;
    off_t offset = 0;
    char carry[MAX_LINE_LEN];
    size_t carry_len = 0;

    if (st.st_size > 0) {
        read_range_mmap_or_fd(fd, 0, (size_t)st.st_size, &buf, carry, &carry_len);
        offset = st.st_size;
    }
    sync_metrics_to_shm(cfg->shm, cfg->worker_index, &local_acc, &start_ts, true, false);
    send_status_byte(cfg->status_pipe_fd, 'R');

#if 1
    bool is_live = (cfg->mode == MODE_VIVO);
#else
    bool is_live = (cfg->mode != 0);
#endif

    if (is_live && !worker_stop) {
        int inot_fd = inotify_init1(IN_NONBLOCK);
        int wd = -1;
        if (inot_fd >= 0) {
            wd = inotify_add_watch(inot_fd, cfg->filepath, IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF | IN_ATTRIB);
        }

        while (!worker_stop) {
            struct stat cur_st;
            if (stat(cfg->filepath, &cur_st) == 0) {
                if (cur_st.st_ino != current_ino) {
                    /* Archivo rotado: leer remanente del fd anterior y abrir el nuevo */
                    struct stat old_st;
                    if (fstat(fd, &old_st) == 0 && old_st.st_size > offset) {
                        read_range_mmap_or_fd(fd, offset, (size_t)(old_st.st_size - offset),
                                              &buf, carry, &carry_len);
                    }
                    close(fd);
                    fd = open(cfg->filepath, O_RDONLY);
                    if (fd >= 0) {
                        fstat(fd, &st);
                        current_ino = st.st_ino;
                        offset = 0;
                        if (inot_fd >= 0 && wd >= 0) {
                            inotify_rm_watch(inot_fd, wd);
                            wd = inotify_add_watch(inot_fd, cfg->filepath,
                                                   IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF | IN_ATTRIB);
                        }
                    }
                } else if (cur_st.st_size > offset) {
                    read_range_mmap_or_fd(fd, offset, (size_t)(cur_st.st_size - offset),
                                          &buf, carry, &carry_len);
                    offset = cur_st.st_size;
                }
            }

            sync_metrics_to_shm(cfg->shm, cfg->worker_index, &local_acc, &start_ts, true, false);

            if (inot_fd >= 0) {
                char ev_buf[sizeof(struct inotify_event) * 8];
                ssize_t ev_len = read(inot_fd, ev_buf, sizeof(ev_buf));
                (void)ev_len;
            }
            struct timespec pause = {0, 100000000L}; /* 100 ms */
            nanosleep(&pause, NULL);
        }

        if (wd >= 0 && inot_fd >= 0) {
            inotify_rm_watch(inot_fd, wd);
        }
        if (inot_fd >= 0) {
            close(inot_fd);
        }
    }

    if (carry_len > 0) {
        carry[carry_len] = '\0';
        buffer_push(&buf, carry);
    }

    close(fd);
    buffer_close(&buf);

    for (int i = 0; i < n_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    sync_metrics_to_shm(cfg->shm, cfg->worker_index, &local_acc, &start_ts, false, false);
    send_status_byte(cfg->status_pipe_fd, 'D');

    free(threads);
    free(targs);
    pthread_mutex_destroy(&acc_mutex);
    buffer_destroy(&buf);
    return 0;
}
