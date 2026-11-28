#define _POSIX_C_SOURCE 200809L
#include <unistd.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Uso: %s [-o archivo] [-n lineas] [-r tasa_por_seg] [--rotar [cada_n_lineas]]\n"
            "  -o <archivo>         Archivo de log de salida (por defecto: access_test.log)\n"
            "  -n <lineas>          Cantidad de lineas a generar (0 = infinito, por defecto: 1000)\n"
            "  -r <tasa_por_seg>    Lineas por segundo (0 = sin pausa, por defecto: 0)\n"
            "  --rotar [n]          Rota el archivo cada n lineas (por defecto: 50000)\n",
            prog);
}

static void format_clf_timestamp(char *buf, size_t size, time_t now) {
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(buf, size, "%d/%b/%Y:%H:%M:%S %z", &tm_info);
}

static void sleep_rate(double rate) {
    if (rate <= 0.0) {
        return;
    }
    double interval_sec = 1.0 / rate;
    struct timespec ts;
    ts.tv_sec = (time_t)interval_sec;
    ts.tv_nsec = (long)((interval_sec - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) {
        ts.tv_nsec = 0;
    }
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && keep_running) {
        /* Reintentar si fue interrumpido por señal */
    }
}

static FILE *rotate_log_file(FILE *current_fp, const char *base_path, int *rot_index) {
    if (current_fp) {
        fflush(current_fp);
        fclose(current_fp);
    }

    (*rot_index)++;
    char rotated_path[512];
    snprintf(rotated_path, sizeof(rotated_path), "%s.%d", base_path, *rot_index);

    if (rename(base_path, rotated_path) != 0) {
        perror("rename en rotacion de log");
    }

    FILE *new_fp = fopen(base_path, "a");
    if (!new_fp) {
        perror("fopen tras rotar log");
        return NULL;
    }
    setvbuf(new_fp, NULL, _IOLBF, 0);
    return new_fp;
}

int main(int argc, char **argv) {
    const char *out_path = "access_test.log";
    uint64_t target_lines = 1000;
    double rate = 0.0;
    bool rotate_enabled = false;
    uint64_t rotate_every = 50000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            target_lines = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            rate = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--rotar") == 0) {
            rotate_enabled = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                char *endptr = NULL;
                unsigned long long val = strtoull(argv[i + 1], &endptr, 10);
                if (endptr != argv[i + 1] && *endptr == '\0' && val > 0) {
                    rotate_every = (uint64_t)val;
                    i++;
                }
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Argumento desconocido: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    FILE *fp = fopen(out_path, "a");
    if (!fp) {
        perror("fopen archivo de log");
        return 1;
    }
    setvbuf(fp, NULL, _IOLBF, 0);

    srand((unsigned int)(time(NULL) ^ (unsigned int)getpid()));

    const char *ips[] = {
        "192.168.1.10", "10.0.0.25", "172.16.4.8", "203.0.113.42",
        "198.51.100.7", "192.168.1.55", "10.0.1.102", "45.33.32.156"
    };
    const char *methods[] = {"GET", "POST", "PUT", "DELETE"};
    const char *urls[] = {
        "/", "/index.html", "/api/v1/items", "/api/v1/users",
        "/login", "/static/app.js", "/reports/summary", "/health"
    };
    const char *queries[] = {
        "", "?page=1&limit=20", "?sort=desc", "?debug=true&token=xyz", "?filter=active"
    };
    const int statuses[] = {200, 200, 200, 201, 204, 301, 302, 400, 401, 403, 404, 500, 502, 503};
    const char *referers[] = {
        "-", "https://example.com/", "https://app.example.local/dashboard", "http://localhost:8080/"
    };
    const char *agents[] = {
        "Mozilla/5.0 (X11; Linux x86_64)",
        "curl/8.5.0",
        "LogPulseBot/1.0",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"
    };

    size_t n_ips = sizeof(ips) / sizeof(ips[0]);
    size_t n_methods = sizeof(methods) / sizeof(methods[0]);
    size_t n_urls = sizeof(urls) / sizeof(urls[0]);
    size_t n_queries = sizeof(queries) / sizeof(queries[0]);
    size_t n_statuses = sizeof(statuses) / sizeof(statuses[0]);
    size_t n_referers = sizeof(referers) / sizeof(referers[0]);
    size_t n_agents = sizeof(agents) / sizeof(agents[0]);

    uint64_t generated = 0;
    uint64_t since_rotate = 0;
    int rot_index = 0;

    while (keep_running && (target_lines == 0 || generated < target_lines)) {
        if (rotate_enabled && since_rotate >= rotate_every) {
            fp = rotate_log_file(fp, out_path, &rot_index);
            if (!fp) {
                return 1;
            }
            since_rotate = 0;
        }

        char ts_buf[64];
        format_clf_timestamp(ts_buf, sizeof(ts_buf), time(NULL));

        int roll = rand() % 100;
        const char *ip = ips[(size_t)rand() % n_ips];
        const char *method = methods[(size_t)rand() % n_methods];
        const char *url = urls[(size_t)rand() % n_urls];
        const char *query = queries[(size_t)rand() % n_queries];
        int status = statuses[(size_t)rand() % n_statuses];
        uint64_t bytes = (status == 204 || status == 304) ? 0 : (uint64_t)(128 + rand() % 65536);
        const char *ref = referers[(size_t)rand() % n_referers];
        const char *ua = agents[(size_t)rand() % n_agents];

        if (roll < 5) {
            /* ~5% de líneas mal formadas para probar el conteo de errores */
            fprintf(fp, "%s - - [%s] CORRUPT_LINE_WITHOUT_HTTP_FIELDS %d\n", ip, ts_buf, rand() % 999);
        } else if (roll < 20) {
            /* ~15% de líneas válidas sin tiempo de respuesta final */
            fprintf(fp, "%s - - [%s] \"%s %s%s HTTP/1.1\" %d %llu \"%s\" \"%s\"\n",
                    ip, ts_buf, method, url, query, status, (unsigned long long)bytes, ref, ua);
        } else {
            /* ~80% de líneas válidas con tiempo de respuesta en segundos al final */
            double resp_time = 0.002 + ((double)(rand() % 1500) / 1000.0);
            fprintf(fp, "%s - - [%s] \"%s %s%s HTTP/1.1\" %d %llu \"%s\" \"%s\" %.4f\n",
                    ip, ts_buf, method, url, query, status, (unsigned long long)bytes, ref, ua, resp_time);
        }

        generated++;
        since_rotate++;

        if (rate > 0.0) {
            sleep_rate(rate);
        }
    }

    if (fp) {
        fflush(fp);
        fclose(fp);
    }

    return 0;
}
