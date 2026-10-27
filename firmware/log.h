#ifndef LOG_H
#define LOG_H
#include <stdint.h>

// IRQ-safe line logging over USB CDC. Lines are buffered in a ring and
// drained from the main loop; nothing ever blocks the BLE callback.
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_hexdump(const char *tag, const uint8_t *data, unsigned len);
void log_drain(void);
void log_init(void);

#define LOG(...) log_line(__VA_ARGS__)
#endif
