#include "buffer.h"

int buffer_init(BoundedBuffer *buf) {
    (void)buf;
    return 0;
}

void buffer_destroy(BoundedBuffer *buf) {
    (void)buf;
}

bool buffer_push(BoundedBuffer *buf, const char *line) {
    (void)buf;
    (void)line;
    return false;
}

bool buffer_pop(BoundedBuffer *buf, char *out_line) {
    (void)buf;
    (void)out_line;
    return false;
}

void buffer_close(BoundedBuffer *buf) {
    (void)buf;
}
