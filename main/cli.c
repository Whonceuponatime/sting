#include "cli.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "report.h"
#include "session.h"
#include "store.h"
#include "usb_probe.h"

#define CLI_UART UART_NUM_0
#define CLI_LINE_MAX 80

static bool s_store_ok;

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

static void cmd_report(void)
{
    static session_rec_t rec; // only used from the CLI task
    char label[LABEL_MAX];
    uint16_t count = store_count();

    printf("\n=== STING USB PORT LIVENESS REPORT ===\n");
    printf("firmware : sting %s (built %s %s)\n", STING_VERSION, __DATE__, __TIME__);
    printf("tester   : USB %04x:%04x \"%s\" serial %s\n", USB_VID, USB_PID, USB_PRODUCT, probe_serial());
    printf("window   : %d s observation per session\n", OBSERVE_WINDOW_MS / 1000);
    printf("times    : t+ values are seconds since power-up; a real date is shown per\n");
    printf("           session when a phone set the clock over WiFi\n");
    printf("sessions : %u\n", (unsigned)count);
    if (!s_store_ok) {
        printf("WARNING  : the session log could not be opened at boot; run `clear` to reinitialise it\n");
    }

    for (uint16_t n = 1; n <= count; n++) {
        if (store_read(n, &rec)) {
            report_print_session(&rec);
        } else {
            printf("\n--- Session %u ---\n  (record unreadable or written by a different firmware version)\n",
                   (unsigned)n);
        }
    }

    store_get_label(label, sizeof(label));
    if (label[0] != '\0') {
        printf("\nnext session label: %s\n", label);
    }
    printf("\n=== END OF REPORT ===\n");
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static void cmd_last(void)
{
    static session_rec_t rec;
    uint16_t count = store_count();
    if (count == 0) {
        printf("no sessions stored\n");
        return;
    }
    if (store_read(count, &rec)) { // highest ordinal = newest
        report_print_session(&rec);
    } else {
        printf("\n--- newest session ---\n  (record unreadable or from a different firmware version)\n");
    }
}

static void print_help(void)
{
    printf("commands:\n"
           "  report         print all stored sessions\n"
           "  last           print only the most recent session\n"
           "  clear          wipe all stored sessions\n"
           "  label <name>   tag the next session, e.g. label FDS-USB1\n"
           "  help           show this list\n");
}

static void cmd_clear(void)
{
    esp_err_t err = store_clear();
    if (err == ESP_OK) {
        s_store_ok = true;
        printf("log cleared: 0 sessions stored\n");
    } else {
        printf("ERROR: could not clear the log (%s)\n", esp_err_to_name(err));
    }
}

static void cmd_label(char *arg)
{
    char label[LABEL_MAX];

    while (*arg == ' ') {
        arg++;
    }
    if (*arg == '\0') {
        store_get_label(label, sizeof(label));
        if (label[0] != '\0') {
            printf("next session label: %s\n", label);
        } else {
            printf("no label set. usage: label <name>\n");
        }
        return;
    }
    if (strlen(arg) >= LABEL_MAX) {
        printf("ERROR: label too long (max %d characters)\n", LABEL_MAX - 1);
        return;
    }
    esp_err_t err = store_set_label(arg);
    if (err == ESP_OK) {
        printf("next session will be labelled: %s\n", arg);
    } else {
        printf("ERROR: could not save the label (%s)\n", esp_err_to_name(err));
    }
}

static void run_line(char *line)
{
    // Trim trailing spaces.
    size_t len = strlen(line);
    while (len > 0 && line[len - 1] == ' ') {
        line[--len] = '\0';
    }
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    // Split "command args" and lower-case the command word.
    char *arg = strchr(line, ' ');
    if (arg != NULL) {
        *arg++ = '\0';
    } else {
        arg = line + strlen(line);
    }
    for (char *p = line; *p != '\0'; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    bool is_report = strcmp(line, "report") == 0;
    bool is_last = strcmp(line, "last") == 0;
    bool is_clear = strcmp(line, "clear") == 0;
    bool is_label = strcmp(line, "label") == 0;
    bool is_help = strcmp(line, "help") == 0 || strcmp(line, "?") == 0;

    if (!(is_report || is_last || is_clear || is_label || is_help)) {
        printf("unknown command: %s (type help)\n", line);
        return;
    }

    // A valid command proves a laptop is on the UART port, so this power-up is
    // not a port test: drop its session before doing anything else.
    session_enter_cli_mode();

    if (is_report) {
        cmd_report();
    } else if (is_last) {
        cmd_last();
    } else if (is_clear) {
        cmd_clear();
    } else if (is_label) {
        cmd_label(arg);
    } else {
        print_help();
    }
}

static void cli_task(void *arg)
{
    (void)arg;
    char line[CLI_LINE_MAX + 1];
    size_t len = 0;
    bool last_was_cr = false;

    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CLI_UART, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' && last_was_cr) { // second half of CR LF
            last_was_cr = false;
            continue;
        }
        last_was_cr = (c == '\r');

        if (c == '\r' || c == '\n') {
            printf("\n");
            line[len] = '\0';
            run_line(line);
            len = 0;
            printf("> ");
        } else if (c == 0x08 || c == 0x7F) { // backspace / delete
            if (len > 0) {
                len--;
                printf("\b \b");
            }
        } else if (c >= 0x20 && c < 0x7F && len < CLI_LINE_MAX) {
            line[len++] = (char)c;
            putchar(c); // echo
        }
        fflush(stdout);
    }
}

void cli_start(bool store_ok)
{
    char label[LABEL_MAX];
    s_store_ok = store_ok;

    uart_driver_install(CLI_UART, 1024, 0, 0, NULL, 0);

    printf("\n\nSTING USB Port Liveness Tester v%s\n", STING_VERSION);
    if (store_ok) {
        printf("%u session(s) stored (max %d).\n", (unsigned)store_count(), MAX_SESSIONS);
        store_get_label(label, sizeof(label));
        if (label[0] != '\0') {
            printf("next session label: %s\n", label);
        }
    } else {
        printf("WARNING: the session log could not be opened. Run `clear` to reinitialise it.\n");
    }
    printf("Never connect the USB port and the UART port at the same time.\n");
    print_help();
    printf("> ");
    fflush(stdout);

    xTaskCreate(cli_task, "cli", 6144, NULL, 3, NULL);
}
