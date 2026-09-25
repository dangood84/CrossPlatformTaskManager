#include "posix_features.h"
#include "term.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include <io.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

static int g_inited = 0;

#ifdef _WIN32
static HANDLE g_out = INVALID_HANDLE_VALUE;
static HANDLE g_in = INVALID_HANDLE_VALUE;
static DWORD g_out_mode = 0;
static DWORD g_in_mode = 0;
static int g_got_out_mode = 0;
static int g_got_in_mode = 0;
static UINT g_old_cp = 0;
#else
static struct termios g_old;
static int g_got_old = 0;
#endif

static void restore_atexit(void)
{
    term_restore();
}

int term_is_tty(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdout));
#else
    return isatty(STDOUT_FILENO);
#endif
}

int term_init(void)
{
    if (g_inited) {
        return 0;
    }

#ifdef _WIN32
    /* WORKING: Windows 10+ can speak the same ANSI sequences as a Unix
     * terminal once ENABLE_VIRTUAL_TERMINAL_PROCESSING is on. Without it
     * the dashboard is a mess of escape codes. We also force UTF-8 so the
     * block-character bars and the selection arrow are not "??". */
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    if (g_out != INVALID_HANDLE_VALUE && GetConsoleMode(g_out, &g_out_mode)) {
        DWORD mode = g_out_mode;
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
        SetConsoleMode(g_out, mode);
        g_got_out_mode = 1;
    }
    if (g_in != INVALID_HANDLE_VALUE && GetConsoleMode(g_in, &g_in_mode)) {
        DWORD mode = g_in_mode;
        mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
        mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        SetConsoleMode(g_in, mode);
        g_got_in_mode = 1;
    }
    g_old_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#else
    if (tcgetattr(STDIN_FILENO, &g_old) == 0) {
        struct termios raw = g_old;
        /* WORKING: ICANON+ECHO off so a single 'q' is readable without
         * Enter. ISIG stays on so Ctrl+C still raises SIGINT and the
         * signal handler can restore the terminal. */
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        g_got_old = 1;
    }
#endif

    /* Alt screen + hide cursor. The previous scrollback comes back on
     * restore, so a long run does not leave 400 table frames behind. */
    fputs("\x1b[?1049h\x1b[?25l", stdout);
    fflush(stdout);

    atexit(restore_atexit);
    g_inited = 1;
    return 0;
}

void term_restore(void)
{
    if (!g_inited) {
        return;
    }
    fputs("\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);

#ifdef _WIN32
    if (g_got_out_mode && g_out != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_out, g_out_mode);
    }
    if (g_got_in_mode && g_in != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_in, g_in_mode);
    }
    if (g_old_cp != 0) {
        SetConsoleOutputCP(g_old_cp);
    }
#else
    if (g_got_old) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_old);
    }
#endif
    g_inited = 0;
}

#ifdef _WIN32
static int decode_win_special(int k)
{
    if (k == 72) {
        return TERM_KEY_UP;
    }
    if (k == 80) {
        return TERM_KEY_DOWN;
    }
    if (k == 73) {
        return TERM_KEY_PGUP;
    }
    if (k == 81) {
        return TERM_KEY_PGDN;
    }
    if (k == 71) {
        return TERM_KEY_HOME;
    }
    if (k == 79) {
        return TERM_KEY_END;
    }
    return TERM_KEY_NONE;
}
#else
static int decode_csi(const unsigned char *seq, int n)
{
    if (n < 2 || seq[0] != '[') {
        return TERM_KEY_NONE;
    }
    if (seq[1] == 'A') {
        return TERM_KEY_UP;
    }
    if (seq[1] == 'B') {
        return TERM_KEY_DOWN;
    }
    if (seq[1] == 'H') {
        return TERM_KEY_HOME;
    }
    if (seq[1] == 'F') {
        return TERM_KEY_END;
    }
    if (n >= 3 && seq[1] == '5' && seq[2] == '~') {
        return TERM_KEY_PGUP;
    }
    if (n >= 3 && seq[1] == '6' && seq[2] == '~') {
        return TERM_KEY_PGDN;
    }
    return TERM_KEY_NONE;
}
#endif

int term_poll_key(int timeout_ms)
{
#ifdef _WIN32
    DWORD start = GetTickCount();
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    while ((DWORD)(GetTickCount() - start) < (DWORD)timeout_ms) {
        if (_kbhit()) {
            int c = _getch();
            /* WORKING: function/arrow keys arrive as 0 or 224 plus a
             * follow-up. Decode the pair so an arrow can move the
             * selection instead of looking like a mystery command. */
            if (c == 0 || c == 224) {
                return decode_win_special(_getch());
            }
            if (c == 8) {
                return TERM_KEY_BACKSPACE;
            }
            if (c == 13) {
                return TERM_KEY_ENTER;
            }
            return c;
        }
        Sleep(20);
    }
    return TERM_KEY_NONE;
#else
    struct pollfd p;
    unsigned char c;

    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, timeout_ms) <= 0) {
        return TERM_KEY_NONE;
    }
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return TERM_KEY_NONE;
    }
    if (c == 0x1b) {
        /* WORKING: a lone Esc (filter cancel) vs a CSI arrow. Wait a
         * short moment for the rest of the sequence; if nothing comes,
         * it really was Escape. */
        unsigned char seq[8];
        int n = 0;
        struct pollfd extra;

        extra.fd = STDIN_FILENO;
        extra.events = POLLIN;
        extra.revents = 0;
        if (poll(&extra, 1, 30) <= 0) {
            return TERM_KEY_ESC;
        }
        while (n < 7) {
            extra.revents = 0;
            if (poll(&extra, 1, n == 0 ? 30 : 0) <= 0) {
                break;
            }
            if (read(STDIN_FILENO, &seq[n], 1) != 1) {
                break;
            }
            n++;
        }
        return decode_csi(seq, n);
    }
    if (c == 0x7f || c == 0x08) {
        return TERM_KEY_BACKSPACE;
    }
    if (c == '\n' || c == '\r') {
        return TERM_KEY_ENTER;
    }
    return (int)c;
#endif
}

void term_size(int *cols, int *rows)
{
    int c = 80;
    int r = 24;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (g_out != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(g_out, &info)) {
        c = info.srWindow.Right - info.srWindow.Left + 1;
        r = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        c = ws.ws_col;
        r = ws.ws_row;
    }
#endif
    /* WORKING: do not pretend a windowed Mac terminal is 60×16 when it
     * is 80×24 (or 50×18). The old floor made us paint lines wider
     * than the glass; they wrapped and the CPU/memory header scrolled
     * off the top. Honour the real size, with only a tiny floor so a
     * 1×1 ioctl glitch does not collapse the layout. */
    if (cols) {
        *cols = util_clamp_int(c, 40, 240);
    }
    if (rows) {
        *rows = util_clamp_int(r, 10, 120);
    }
}

void term_begin_frame(void)
{
    /* Home only. Erasing the display here flashes on Windows cmd
     * (see render.c). Live frames are composed off-screen instead. */
    fputs("\x1b[H", stdout);
}
