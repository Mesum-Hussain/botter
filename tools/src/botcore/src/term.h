#ifndef BC_TERM_H
#define BC_TERM_H

#define ANSI_RESET     "\033[0m"
#define ANSI_RED       "\033[31m"
#define ANSI_BLUE      "\033[34m"
#define ANSI_BOLD_RED  "\033[1;31m"
#define ANSI_BOLD_BLUE "\033[1;34m"
#define ANSI_DIM       "\033[2m"
#define ANSI_LGREEN    "\033[92m"  /* text the user types */

#define PROMPT_USER   ANSI_RED  ">> " ANSI_RESET
#define PROMPT_AGENT  ANSI_BLUE ">> " ANSI_RESET

#define TERM_LINE   1
#define TERM_EOF    0
#define TERM_INTR  (-1)
#define TERM_TICK   2   /* idle callback asked the caller to run something; typed text is kept */

/* Install signal handlers; restores tty echo on exit/termination. */
void term_init(void);

/*
 * Print prompt, read one line (no trailing newline) into *out (malloc'd).
 * secret != 0 disables echo. On a tty (non-secret) a small line editor
 * provides arrow keys, Home/End, Delete, Ctrl-A/E/K/U/W and history. Returns TERM_LINE, TERM_EOF, or TERM_INTR
 * (Ctrl-C at the prompt).
 */
int  term_readline(const char *prompt, int secret, char **out);

/* Ctrl-C flag, set by the SIGINT handler. Long-running work polls it. */
int  term_interrupted(void);
void term_clear_interrupt(void);

/*
 * While a prompt is waiting for a key (about once a second) cb() is called;
 * a nonzero return makes term_readline return TERM_TICK. Pass NULL to disable.
 * Only the line editor (tty input) honours it.
 */
void term_set_idle(int (*cb)(void));

/* Remember a line for Up/Down recall in the line editor (RAM only). */
void term_history_add(const char *line);

/* Write text to stdout, dropping ESC and other control chars (except \n, \t). */
void term_print_clean(const char *s);

#endif
