#include "ui.h"

void ui_init(void) {}

void ui_render(const SharedMemoryData *shm, RunMode mode, bool paused, const char *last_event) {
    (void)shm;
    (void)mode;
    (void)paused;
    (void)last_event;
}

int ui_poll_key(void) {
    return -1;
}

void ui_cleanup(void) {}
