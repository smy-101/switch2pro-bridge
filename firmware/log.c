#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pico/critical_section.h"
#include "tusb.h"

#define LOG_RING_SIZE 8192
static char log_ring[LOG_RING_SIZE];
static volatile uint32_t log_head, log_tail;
static critical_section_t log_cs;
static bool log_cs_ready;

void log_line(const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((unsigned)n >= sizeof(buf)) n = sizeof(buf) - 1;
    buf[n] = '\n';

    if (!log_cs_ready) {
        // too early (pre-init) — drop
        return;
    }
    critical_section_enter_blocking(&log_cs);
    for (int i = 0; i <= n; i++) {
        uint32_t next = (log_head + 1) % LOG_RING_SIZE;
        if (next == log_tail) break;               // full: drop
        log_ring[log_head] = buf[i];
        log_head = next;
    }
    critical_section_exit(&log_cs);
}

void log_hexdump(const char *tag, const uint8_t *data, unsigned len) {
    char line[64];
    unsigned i = 0;
    log_line("%s (%u bytes):", tag, len);
    while (i < len) {
        unsigned o = 0;
        unsigned j = i;
        for (; j < len && j < i + 16; j++) {
            o += (unsigned)snprintf(line + o, sizeof(line) - o, "%02x ", data[j]);
        }
        log_line("  %04x  %s", i, line);
        i = j;
    }
}

void log_drain(void) {
    if (!log_cs_ready) return;
    // move up to 256 bytes per call into CDC
    static char out[256];
    unsigned n = 0;
    critical_section_enter_blocking(&log_cs);
    while (log_tail != log_head && n < sizeof(out)) {
        out[n++] = log_ring[log_tail];
        log_tail = (log_tail + 1) % LOG_RING_SIZE;
    }
    critical_section_exit(&log_cs);
    if (n && tud_ready() && tud_cdc_connected()) {
        tud_cdc_write(out, n);
        tud_cdc_write_flush();
    }
}

void log_init(void) {
    critical_section_init(&log_cs);
    log_cs_ready = true;
}
