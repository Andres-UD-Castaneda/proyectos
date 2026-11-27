#include <string.h>
#include "parser.h"

void parse_log_line(const char *line, LogEntry *entry) {
    (void)line;
    if (entry) {
        memset(entry, 0, sizeof(*entry));
        entry->response_time = -1.0;
        entry->valid = false;
    }
}
