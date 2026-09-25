#ifndef TERM_H
#define TERM_H

/* Raw-ish terminal: alt screen, hidden cursor, non-blocking key reads.
 * --once and make test never call these; they print to a normal stdout. */

#define TERM_KEY_NONE      0
#define TERM_KEY_ESC      27
#define TERM_KEY_ENTER    13
#define TERM_KEY_BACKSPACE 127
#define TERM_KEY_UP     1001
#define TERM_KEY_DOWN   1002
#define TERM_KEY_PGUP   1003
#define TERM_KEY_PGDN   1004
#define TERM_KEY_HOME   1005
#define TERM_KEY_END    1006

int  term_init(void);
void term_restore(void);
int  term_poll_key(int timeout_ms);   /* 0 if none; ASCII; TERM_KEY_* */
void term_size(int *cols, int *rows); /* defaults 80x24 if ioctl fails */
void term_begin_frame(void);
int  term_is_tty(void);

#endif /* TERM_H */
