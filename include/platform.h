/*
 * platform.h — OS/platform abstraction layer
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Platform detection ─────────────────────────────────────────── */

#if defined(__linux__)
  #define PLATFORM_LINUX 1
#elif defined(__APPLE__) && defined(__MACH__)
  #define PLATFORM_MACOS 1
#elif defined(_WIN32) || defined(_WIN64)
  #define PLATFORM_WINDOWS 1
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
  #endif
#else
  #warning "Unknown platform"
#endif

/* ── Platform info ──────────────────────────────────────────────── */

/* Human-readable OS name */
const char *os_name(void);

/* OS architecture string */
const char *os_arch(void);

/* ── Environment ────────────────────────────────────────────────── */

/* Get environment variable (returns NULL if not set).
   Caller must free the returned string. */
char *os_getenv(const char *name);

/* Set environment variable. Returns 0 on success, -1 on error. */
int os_setenv(const char *name, const char *value);

/* ── Paths ──────────────────────────────────────────────────────── */

/* Path separator for this platform ("/" or "\\") */
const char *os_path_sep(void);

/* Get the user's home directory */
char *os_home_dir(void);

/* Get the user's config directory (~/.config on Linux) */
char *os_config_dir(void);

/* Build a path from components. Result must be freed. */
char *os_path_join(const char *a, const char *b);

/* Check if a file/directory exists */
bool os_path_exists(const char *path);

/* Check if path is a directory */
bool os_is_dir(const char *path);

/* Create directory (with parents if needed) */
int os_mkdir_p(const char *path);

/* ── Process ────────────────────────────────────────────────────── */

/* Execute a command and capture stdout. Returns malloc'd string or NULL.
   Sets *exit_code to the command's exit status. */
char *os_exec_capture(const char *command, int *exit_code);

/* ── Time ───────────────────────────────────────────────────────── */

/* Get monotonic timestamp in milliseconds */
int64_t os_time_ms(void);

/* ── UTF-8 aware terminal input ──────────────────────────────────── */

/* Read a line from stdin with UTF-8 aware editing.
 * Handles backspace (full UTF-8 character deletion), Ctrl-D (EOF),
 * and basic cursor movement.
 * Returns a malloc'd string (caller frees), or NULL on EOF. */
char *utf8_readline(const char *prompt);

#endif /* PLATFORM_H */
