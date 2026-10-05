/* Progress reporting: progress bar and --machine-progress output. */
#include "flashfmt.h"

#include <signal.h>
#include <sys/ioctl.h>

/* Progress reporting: percent = 100 * done / total,
 * reported at the start of every padded write and after each chunk. */
const char* prog_msg = "";
static unsigned short term_cols = 80;
static volatile sig_atomic_t winch_pending;

void on_winch(int sig) {
    if (sig == SIGWINCH) winch_pending = 1;
}

void refresh_term_cols(void) {
    struct winsize ws;
    if (ioctl(fileno(stdout), TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20)
        term_cols = ws.ws_col;
}

/* phase line + "[=====-    ]  42 %"; cursor goes back up for the next redraw */
static void render_bar(unsigned pct) {
    if (winch_pending) {
        refresh_term_cols();
        winch_pending = 0;
    }
    printf("\r%s\033[K\n", pct > 99 ? "Done" : prog_msg);
    unsigned w = term_cols - 8u, full = pct * w / 100, i;
    fputs("\r[", stdout);
    for (i = 0; i < full; i++) putc('=', stdout);
    if (pct * w % 100 != 0 && i < w) {
        putc('-', stdout);
        i++;
    }
    for (; i < w; i++) putc(' ', stdout);
    printf("] %3u %%%s", pct, pct == 100 ? "\n" : "\r\033[A");
    fflush(stdout);
}

void progress(uint64_t n) {
    static unsigned last_pct = ~0u;
    static const char* last_msg;
    dev.done += n;
    if (!dev.total) return;
    unsigned pct = (unsigned)(100 * dev.done / dev.total);
    if (pct == last_pct && prog_msg == last_msg) return;
    last_pct = pct;
    last_msg = prog_msg;
    if (opt.machine_progress) {
        printf("PROGRESS:%u/100 %s\n", pct, prog_msg);
        fflush(stdout);
    } else if (opt.progress_bar == 1) {
        render_bar(pct);
    }
}
