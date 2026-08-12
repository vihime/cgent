/*
 * interrupt.h — Graceful SIGINT (Ctrl-C) cancellation
 *
 * Installs a SIGINT handler that sets a flag instead of terminating the
 * process. Long-running loops (HTTP reads, command execution, agent
 * turns) check the flag and abort cleanly, so the REPL can return to
 * the prompt instead of dying.
 */
#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <stdbool.h>

/* Install the SIGINT handler. Call once at startup. */
void interrupt_init(void);

/* True if the user pressed Ctrl-C since the last interrupt_clear(). */
bool interrupt_requested(void);

/* Reset the flag (e.g. after the interrupt was handled). */
void interrupt_clear(void);

#endif /* INTERRUPT_H */
