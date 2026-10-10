#ifndef BC_FRONT_H
#define BC_FRONT_H

/*
 * Frontend protocol: lets a separate UI program drive botcore over pipes.
 * Enabled only when the process starts with BOTCORE_FRONTEND=1 (the variable is
 * removed so tools never see it). Without it nothing changes: plain REPL.
 *
 * botcore -> UI (stdout; stderr is merged into it so order is kept):
 *   ordinary output as before (may contain SGR colour codes), interleaved with
 *   records "\x1e" + one-line JSON object + "\n". Records:
 *     {"ev":"prompt","text":<prompt>,"secret":bool}   waiting for one input line
 *     {"ev":"busy","label":<text>} / {"ev":"idle"}    work in progress / done
 *     {"ev":"info","model":..,"cwd":..}                session details
 *     {"ev":"thinking","text":..}                      model reasoning
 *     {"ev":"text","text":..}                          assistant text sent alongside tool calls
 *     {"ev":"reply","text":..}                         final answer (markdown)
 *     {"ev":"tool","name":..,"args":..,"status":..} / {"ev":"tool_result","text":..}
 *     {"ev":"delta","kind":"text"|"thinking","text":..}  streamed piece of a reply
 *     {"ev":"mode","mode":"plan"|"build"}             Plan/Build mode
 *     {"ev":"flow"}                                   the agent's FLOW.md flow starts
 * UI -> botcore (stdin, one JSON object per line, answers a prompt):
 *     {"line":<text>} | {"eof":true} | {"intr":true}
 *   Interrupting work = SIGINT to the botcore process.
 */

int  front_init(void);   /* 1 if frontend mode is on */
int  front_active(void);
/* Emit a record: event name, then key/value string pairs, NULL-terminated. */
void front_event(const char *ev, ...) __attribute__((sentinel));
/* term_readline() replacement used in frontend mode (same return codes). */
int  front_readline(const char *prompt, int secret, int (*idle)(void), char **out);

#endif
