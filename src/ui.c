#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"

static bool ui_initialized = false;

void ui_init(void) {
    if (ui_initialized) {
        return;
    }
    if (!initscr()) {
        return;
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);   /* Encabezados */
        init_pair(2, COLOR_GREEN, -1);  /* OK / 2xx / Activo */
        init_pair(3, COLOR_YELLOW, -1); /* 3xx / 4xx / Pausa */
        init_pair(4, COLOR_RED, -1);    /* 5xx / Error / Crashed */
    }

    ui_initialized = true;
}

void ui_cleanup(void) {
    if (!ui_initialized) {
        return;
    }
    endwin();
    ui_initialized = false;
}

int ui_poll_key(void) {
    if (!ui_initialized) {
        return ERR;
    }
    return getch();
}

static void format_bytes(uint64_t bytes, char *out, size_t cap) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double val = (double)bytes;
    int u = 0;
    while (val >= 1024.0 && u < 4) {
        val /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
    } else {
        snprintf(out, cap, "%.2f %s", val, units[u]);
    }
}

static void merge_top_pool(StatItem *pool, int *pool_count, const StatItem *src, int src_count) {
    for (int i = 0; i < src_count; i++) {
        if (src[i].key[0] == '\0' || src[i].count == 0) {
            continue;
        }
        bool found = false;
        for (int j = 0; j < *pool_count; j++) {
            if (strcmp(pool[j].key, src[i].key) == 0) {
                pool[j].count += src[i].count;
                pool[j].total_time += src[i].total_time;
                found = true;
                break;
            }
        }
        if (!found && *pool_count < MAX_TRACKED_ITEMS) {
            int idx = (*pool_count)++;
            pool[idx] = src[i];
        }
    }
}

void ui_render(const SharedMemoryData *shm, RunMode mode, bool paused, const char *last_event) {
    if (!ui_initialized || !shm) {
        return;
    }

    SharedMemoryData snap;
    memset(&snap, 0, sizeof(snap));

    SharedMemoryData *mshm = (SharedMemoryData *)shm;
    while (sem_wait(&mshm->mutex) != 0) {
        if (errno != EINTR) {
            break;
        }
    }
    memcpy(&snap, shm, sizeof(snap));
    sem_post(&mshm->mutex);

    uint64_t tot_req = 0, tot_mal = 0, tot_bytes = 0;
    uint64_t s2 = 0, s3 = 0, s4 = 0, s5 = 0;
    double sum_time = 0.0;
    uint64_t timed_count = 0;
    double max_elapsed = 0.0;

    StatItem *all_urls = calloc(MAX_TRACKED_ITEMS, sizeof(StatItem));
    StatItem *all_ips = calloc(MAX_TRACKED_ITEMS, sizeof(StatItem));
    int all_url_n = 0;
    int all_ip_n = 0;

    int n_workers = snap.num_workers;
    if (n_workers < 0) {
        n_workers = 0;
    }
    if (n_workers > MAX_FILES) {
        n_workers = MAX_FILES;
    }

    for (int i = 0; i < n_workers; i++) {
        const WorkerMetrics *wm = &snap.workers[i];
        tot_req += wm->total_requests;
        tot_mal += wm->malformed_lines;
        tot_bytes += wm->bytes_transferred;
        s2 += wm->status_2xx;
        s3 += wm->status_3xx;
        s4 += wm->status_4xx;
        s5 += wm->status_5xx;
        if (wm->elapsed_seconds > max_elapsed) {
            max_elapsed = wm->elapsed_seconds;
        }

        if (all_urls) {
            merge_top_pool(all_urls, &all_url_n, wm->urls, wm->url_count);
            for (int u = 0; u < wm->url_count; u++) {
                sum_time += wm->urls[u].total_time;
                timed_count += wm->urls[u].count;
            }
        }
        if (all_ips) {
            merge_top_pool(all_ips, &all_ip_n, wm->ips, wm->ip_count);
        }
    }

    StatItem top_urls[TOP_N];
    StatItem top_ips[TOP_N];
    memset(top_urls, 0, sizeof(top_urls));
    memset(top_ips, 0, sizeof(top_ips));

    int n_top_urls = (all_urls) ? metrics_top_items(all_urls, all_url_n, top_urls, TOP_N) : 0;
    int n_top_ips = (all_ips) ? metrics_top_items(all_ips, all_ip_n, top_ips, TOP_N) : 0;

    free(all_urls);
    free(all_ips);

    erase();
    int max_y = 0, max_x = 0;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_y;

    char bytes_str[32];
    format_bytes(tot_bytes, bytes_str, sizeof(bytes_str));

    double avg_ms = (timed_count > 0) ? ((sum_time / (double)timed_count) * 1000.0) : 0.0;
    double rps = (max_elapsed > 0.001) ? ((double)tot_req / max_elapsed) : 0.0;

#if 1
    const char *mode_label = (mode == MODE_VIVO) ? "EN VIVO" : "LOTE";
#else
    const char *mode_label = (mode != 0) ? "EN VIVO" : "LOTE";
#endif

    attron(COLOR_PAIR(1) | A_BOLD);
    mvprintw(0, 1, "LOGPULSE - Monitor Concurrente de Logs [%s]%s",
             mode_label, paused ? " [PAUSADO]" : "");
    attroff(COLOR_PAIR(1) | A_BOLD);

    mvprintw(1, 1, "Req: %llu | Malformadas: %llu | Bytes: %s | RPS: %.1f | Lat. prom: %.2f ms",
             (unsigned long long)tot_req,
             (unsigned long long)tot_mal,
             bytes_str,
             rps,
             avg_ms);

    mvprintw(2, 1, "HTTP ");
    attron(COLOR_PAIR(2));
    printw("2xx:%llu ", (unsigned long long)s2);
    attroff(COLOR_PAIR(2));
    attron(COLOR_PAIR(3));
    printw("3xx:%llu 4xx:%llu ", (unsigned long long)s3, (unsigned long long)s4);
    attroff(COLOR_PAIR(3));
    attron(COLOR_PAIR(4));
    printw("5xx:%llu", (unsigned long long)s5);
    attroff(COLOR_PAIR(4));

    int row = 4;
    attron(A_BOLD);
    mvprintw(row++, 1, "WORKERS (%d):", n_workers);
    attroff(A_BOLD);

    int max_w_rows = (n_workers < 6) ? n_workers : 6;
    for (int i = 0; i < max_w_rows; i++) {
        const WorkerMetrics *wm = &snap.workers[i];
        const char *st_txt = wm->crashed ? "CRASH" : (wm->active ? "ACTIVO" : "LISTO");
        int pair_id = wm->crashed ? 4 : (wm->active ? 2 : 1);
        attron(COLOR_PAIR(pair_id));
        mvprintw(row, 2, "[W%d PID:%d %-7s]", i, (int)wm->pid, st_txt);
        attroff(COLOR_PAIR(pair_id));
        printw(" %-26.26s req:%llu err:%llu",
               wm->filename[0] ? wm->filename : "-",
               (unsigned long long)wm->total_requests,
               (unsigned long long)wm->malformed_lines);
        row++;
    }

    row++;
    int half_x = (max_x > 40) ? (max_x / 2) : 40;

    attron(A_BOLD);
    mvprintw(row, 1, "TOP %d URLS", TOP_N);
    mvprintw(row, half_x, "TOP %d IPS", TOP_N);
    attroff(A_BOLD);
    row++;

    int top_rows = (n_top_urls > n_top_ips) ? n_top_urls : n_top_ips;
    if (top_rows > TOP_N) {
        top_rows = TOP_N;
    }

    for (int i = 0; i < top_rows; i++) {
        if (i < n_top_urls) {
            double u_avg = (top_urls[i].count > 0)
                               ? ((top_urls[i].total_time / (double)top_urls[i].count) * 1000.0)
                               : 0.0;
            mvprintw(row + i, 1, "%2d. %-22.22s %6llu (%.1fms)",
                     i + 1, top_urls[i].key,
                     (unsigned long long)top_urls[i].count, u_avg);
        }
        if (i < n_top_ips) {
            mvprintw(row + i, half_x, "%2d. %-16.16s %6llu",
                     i + 1, top_ips[i].key,
                     (unsigned long long)top_ips[i].count);
        }
    }

    int footer_row = row + TOP_N + 1;
    mvprintw(footer_row, 1, "Evento: %s", (last_event && last_event[0]) ? last_event : "Ninguno");
    mvprintw(footer_row + 1, 1, "Teclas: [p] Pausar/Reanudar  [e] Exportar CSV  [r] Reiniciar  [q] Salir");

    refresh();
}
