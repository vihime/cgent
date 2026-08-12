/*
 * interrupt.c — SIGINT handling for graceful cancellation
 */
#include "interrupt.h"

#include <signal.h>
#include <string.h>

static volatile sig_atomic_t g_interrupted = 0;

static void interrupt_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

void interrupt_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = interrupt_handler;
    /* No SA_RESTART: blocking reads return EINTR so loops can react */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
}

bool interrupt_requested(void) {
    return g_interrupted != 0;
}

void interrupt_clear(void) {
    g_interrupted = 0;
}
