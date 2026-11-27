#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include "common.h"
#include "metrics.h"

void ui_init(void);
void ui_render(const SharedMemoryData *shm, RunMode mode, bool paused, const char *last_event);
int ui_poll_key(void);
void ui_cleanup(void);

#endif
