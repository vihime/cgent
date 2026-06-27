/*
 * os.c — Platform abstraction implementation (Linux primary)
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#ifdef PLATFORM_LINUX
  #include <sys/sysinfo.h>
#endif

/* ── Platform info ──────────────────────────────────────────────── */

const char *os_name(void) {
#if defined(PLATFORM_LINUX)
    return "linux";
#elif defined(PLATFORM_MACOS)
    return "macos";
#elif defined(PLATFORM_WINDOWS)
    return "windows";
#else
    return "unknown";
#endif
}

const char *os_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

/* ── Environment ────────────────────────────────────────────────── */

char *os_getenv(const char *name) {
    const char *val = getenv(name);
    if (!val) return NULL;
    return strdup(val);
}

int os_setenv(const char *name, const char *value) {
    return setenv(name, value, 1);
}

/* ── Paths ──────────────────────────────────────────────────────── */

const char *os_path_sep(void) {
#ifdef PLATFORM_WINDOWS
    return "\\";
#else
    return "/";
#endif
}

char *os_home_dir(void) {
    const char *home = getenv("HOME");
#ifdef PLATFORM_WINDOWS
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) return strdup(".");
    return strdup(home);
}

char *os_config_dir(void) {
    char *home = os_home_dir();
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char *dir;

    if (xdg && xdg[0]) {
        dir = strdup(xdg);
    } else {
#ifdef PLATFORM_MACOS
        dir = os_path_join(home, "Library/Application Support");
#elif defined(PLATFORM_WINDOWS)
        dir = os_path_join(home, "AppData/Roaming");
#else
        dir = os_path_join(home, ".config");
#endif
    }

    free(home);
    return dir;
}

char *os_path_join(const char *a, const char *b) {
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    int need_sep = 0;

    if (alen == 0) return strdup(b);
    if (blen == 0) return strdup(a);

#ifdef PLATFORM_WINDOWS
    if (a[alen - 1] != '\\' && a[alen - 1] != '/' && b[0] != '\\' && b[0] != '/')
        need_sep = 1;
#else
    if (a[alen - 1] != '/' && b[0] != '/')
        need_sep = 1;
#endif

    char *result = malloc(alen + blen + (need_sep ? 2 : 1));
    if (!result) return NULL;

    memcpy(result, a, alen);
    if (need_sep) {
#ifdef PLATFORM_WINDOWS
        result[alen] = '\\';
#else
        result[alen] = '/';
#endif
        memcpy(result + alen + 1, b, blen + 1);
    } else {
        memcpy(result + alen, b, blen + 1);
    }

    return result;
}

bool os_path_exists(const char *path) {
    return access(path, F_OK) == 0;
}

bool os_is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

int os_mkdir_p(const char *path) {
    char *tmp = strdup(path);
    if (!tmp) return -1;

    size_t len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }

    free(tmp);
    return 0;
}

/* ── Process ────────────────────────────────────────────────────── */

char *os_exec_capture(const char *command, int *exit_code) {
    FILE *fp = popen(command, "r");
    if (!fp) {
        if (exit_code) *exit_code = -1;
        return NULL;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        pclose(fp);
        if (exit_code) *exit_code = -1;
        return NULL;
    }

    while (!feof(fp)) {
        if (len + 4096 >= cap) {
            cap *= 2;
            char *newbuf = realloc(buf, cap);
            if (!newbuf) {
                free(buf);
                pclose(fp);
                if (exit_code) *exit_code = -1;
                return NULL;
            }
            buf = newbuf;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        if (n == 0) break;
        len += n;
    }
    buf[len] = '\0';

    int rc = pclose(fp);
    if (exit_code) {
#ifdef PLATFORM_WINDOWS
        *exit_code = rc;
#else
        *exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    }

    return buf;
}

/* ── Time ───────────────────────────────────────────────────────── */

int64_t os_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
