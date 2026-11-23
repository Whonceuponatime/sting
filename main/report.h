// Text rendering of one stored session.
#pragma once

#include <stddef.h>

#include "session.h"

// Print one session to stdout (the UART report).
void report_print_session(const session_rec_t *r);

// Render one session into `buf` (NUL-terminated, truncated to `cap`). Returns
// the number of bytes written, excluding the NUL. Same text as the UART report.
size_t report_session_to_buf(const session_rec_t *r, char *buf, size_t cap);
