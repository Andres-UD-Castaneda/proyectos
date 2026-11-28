#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "common.h"
#include <ncurses.h>
#include "metrics.h"
#include "ui.h"
#include "worker.h"

#define DEFAULT_PARSERS 2

typedef struct {
    pid_t pid;
    int pipe_read_fd;
    char filepath[256];
    bool finished;
    int restarts;
} WorkerSlot;

static volatile sig_atomic_t main_stop = 0;

static void handle_main_signal(int sig) {
    (void)sig;
    main_stop = 1;
}

static void write_pipe_msg(int fd, int idx, PipeMsgType type, const char *detail) {
    if (fd < 0) {
        return;
    }
    PipeMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.worker_index = idx;
    msg.pid = getpid();
    msg.type = type;
    if (detail) {
        snprintf(msg.detail, sizeof(msg.detail), "%s", detail);
    }
    ssize_t w;
    do {
        w = write(fd, &msg, sizeof(msg));
    } while (w == -1 && errno == EINTR);
}

static pid_t spawn_worker(WorkerSlot *slot, int idx, RunMode mode, int num_parsers, SharedMemoryData *shm) {
    int pfd[2];
    if (pipe(pfd) != 0) {
        perror("pipe en spawn_worker");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork en spawn_worker");
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pfd[0]);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        write_pipe_msg(pfd[1], idx, MSG_STARTED, slot->filepath);

        WorkerConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.worker_index = idx;
        cfg.filepath = slot->filepath;
        cfg.mode = mode;
        cfg.num_parsers = num_parsers;
        cfg.status_pipe_fd = pfd[1];
        cfg.shm = shm;

        int rc = run_worker_process(&cfg);
        if (rc != 0) {
            write_pipe_msg(pfd[1], idx, MSG_ERROR, "Error en proceso trabajador");
            close(pfd[1]);
            _exit(EXIT_FAILURE);
        }

        write_pipe_msg(pfd[1], idx, MSG_FINISHED, "Completado");
        close(pfd[1]);
        _exit(EXIT_SUCCESS);
    }

    close(pfd[1]);
    int flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
    }

    slot->pid = pid;
    slot->pipe_read_fd = pfd[0];
    slot->finished = false;
    return pid;
}

static void drain_worker_pipes(WorkerSlot *slots, int count, char *last_event, size_t ev_cap) {
    for (int i = 0; i < count; i++) {
        if (slots[i].pipe_read_fd < 0) {
            continue;
        }
        PipeMessage msg;
        ssize_t r;
        while ((r = read(slots[i].pipe_read_fd, &msg, sizeof(msg))) > 0) {
            if ((size_t)r == sizeof(msg)) {
                const char *tag = "EVENTO";
                if (msg.type == MSG_STARTED) {
                    tag = "INICIO";
                } else if (msg.type == MSG_FINISHED) {
                    tag = "FIN";
                } else if (msg.type == MSG_ERROR) {
                    tag = "ERROR";
                } else if (msg.type == MSG_ROTATED) {
                    tag = "ROTACION";
                } else if (msg.type == MSG_DIED) {
                    tag = "CAIDA";
                }
                snprintf(last_event, ev_cap, "[W%d PID:%d] %s: %s",
                         msg.worker_index, (int)msg.pid, tag, msg.detail);
            }
        }
    }
}

static void reset_all_metrics(SharedMemoryData *shm, const WorkerSlot *slots, int count) {
    if (!shm) {
        return;
    }
    while (sem_wait(&shm->mutex) != 0) {
        if (errno != EINTR) {
            break;
        }
    }
    for (int i = 0; i < count && i < MAX_FILES; i++) {
        metrics_worker_reset(&shm->workers[i], slots[i].pid, slots[i].filepath, !slots[i].finished);
    }
    sem_post(&shm->mutex);
}

int main(int argc, char *argv[]) {
    RunMode mode = MODE_LOTE;
    int num_parsers = DEFAULT_PARSERS;
    char files[MAX_FILES][256];
    int file_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--vivo") == 0) {
            mode = MODE_VIVO;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--lote") == 0) {
            mode = MODE_LOTE;
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) && i + 1 < argc) {
            int val = atoi(argv[++i]);
            if (val > 0 && val <= 64) {
                num_parsers = val;
            }
        } else if (argv[i][0] != '-') {
            if (file_count < MAX_FILES) {
                snprintf(files[file_count], sizeof(files[file_count]), "%s", argv[i]);
                file_count++;
            }
        }
    }

    if (file_count == 0) {
        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob("*.log", 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc && file_count < MAX_FILES; i++) {
                snprintf(files[file_count], sizeof(files[file_count]), "%s", g.gl_pathv[i]);
                file_count++;
            }
            globfree(&g);
        }
    }

    if (file_count == 0) {
        fprintf(stderr, "Uso: %s [-v|--vivo | -l|--lote] [-t hilos] <archivo.log> [archivo2.log ...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_main_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    shm_unlink(SHM_NAME);
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return EXIT_FAILURE;
    }

    if (ftruncate(shm_fd, (off_t)sizeof(SharedMemoryData)) != 0) {
        perror("ftruncate");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return EXIT_FAILURE;
    }

    void *shm_ptr = mmap(NULL, sizeof(SharedMemoryData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap en shm");
        shm_unlink(SHM_NAME);
        return EXIT_FAILURE;
    }

    SharedMemoryData *shm = (SharedMemoryData *)shm_ptr;
    metrics_shm_init(shm, file_count);

    WorkerSlot slots[MAX_FILES];
    memset(slots, 0, sizeof(slots));

    for (int i = 0; i < file_count; i++) {
        slots[i].pipe_read_fd = -1;
        snprintf(slots[i].filepath, sizeof(slots[i].filepath), "%s", files[i]);
        spawn_worker(&slots[i], i, mode, num_parsers, shm);
    }

    ui_init();

    bool paused = false;
    char last_event[256] = "Iniciando procesos trabajadores...";

    while (!main_stop) {
        drain_worker_pipes(slots, file_count, last_event, sizeof(last_event));

        int status = 0;
        pid_t exited_pid;
        while ((exited_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < file_count; i++) {
                if (slots[i].pid == exited_pid) {
                    if (slots[i].pipe_read_fd >= 0) {
                        close(slots[i].pipe_read_fd);
                        slots[i].pipe_read_fd = -1;
                    }
                    bool abnormal = WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
                    while (sem_wait(&shm->mutex) != 0) {
                        if (errno != EINTR) {
                            break;
                        }
                    }
                    shm->workers[i].active = false;
                    shm->workers[i].crashed = abnormal;
                    sem_post(&shm->mutex);

                    if (abnormal && mode == MODE_VIVO && !main_stop && slots[i].restarts < 5) {
                        slots[i].restarts++;
                        snprintf(last_event, sizeof(last_event),
                                 "Worker %d cayó (PID %d); reintento #%d...",
                                 i, (int)exited_pid, slots[i].restarts);
                        spawn_worker(&slots[i], i, mode, num_parsers, shm);
                    } else {
                        slots[i].finished = true;
                        snprintf(last_event, sizeof(last_event),
                                 "Worker %d (PID %d) %s",
                                 i, (int)exited_pid, abnormal ? "finalizó con error" : "terminó");
                    }
                    break;
                }
            }
        }

        int ch = ui_poll_key();
        if (ch != ERR) {
            if (ch == 'q' || ch == 'Q') {
                main_stop = 1;
            } else if (ch == 'p' || ch == 'P') {
                paused = !paused;
                snprintf(last_event, sizeof(last_event),
                         paused ? "Vista pausada" : "Vista reanudada");
            } else if (ch == 'e' || ch == 'E') {
                const char *csv_name = "logpulse_metrics.csv";
                if (metrics_export_csv(shm, csv_name) == 0) {
                    snprintf(last_event, sizeof(last_event), "Exportado a %s", csv_name);
                } else {
                    snprintf(last_event, sizeof(last_event), "Error al exportar %s", csv_name);
                }
            } else if (ch == 'r' || ch == 'R') {
                reset_all_metrics(shm, slots, file_count);
                snprintf(last_event, sizeof(last_event), "Métricas reiniciadas");
            }
        }

        if (!paused) {
            ui_render(shm, mode, paused, last_event);
        }

        if (mode == MODE_LOTE) {
            bool all_done = true;
            for (int i = 0; i < file_count; i++) {
                if (!slots[i].finished) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) {
                ui_render(shm, mode, paused, "Análisis en lote completado. Pulsa [q] o espera 2s...");
                struct timespec wait_end = {2, 0};
                nanosleep(&wait_end, NULL);
                break;
            }
        }

        struct timespec tick = {0, 150000000L}; /* 150 ms */
        nanosleep(&tick, NULL);
    }

    ui_cleanup();

    for (int i = 0; i < file_count; i++) {
        if (!slots[i].finished && slots[i].pid > 0) {
            kill(slots[i].pid, SIGTERM);
        }
    }
    for (int i = 0; i < file_count; i++) {
        if (slots[i].pid > 0) {
            waitpid(slots[i].pid, NULL, 0);
        }
        if (slots[i].pipe_read_fd >= 0) {
            close(slots[i].pipe_read_fd);
            slots[i].pipe_read_fd = -1;
        }
    }

    metrics_shm_destroy(shm);
    munmap(shm_ptr, sizeof(SharedMemoryData));
    shm_unlink(SHM_NAME);

    return EXIT_SUCCESS;
}
