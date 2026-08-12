/*
 * os.c — Platform abstraction implementation (Linux primary)
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
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

#ifndef PLATFORM_WINDOWS

/* Maximum bytes captured from a single command (protects the model
 * context from unbounded tool output). */
#define OS_EXEC_CAP (128 * 1024)

char *os_exec_capture_timeout(const char *command, int timeout_ms, int *exit_code) {
    if (!command) {
        if (exit_code) *exit_code = -1;
        return NULL;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        if (exit_code) *exit_code = -1;
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (exit_code) *exit_code = -1;
        return NULL;
    }

    if (pid == 0) {
        /* Child: own process group so the parent can kill the whole
         * command tree on timeout/truncation. */
        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    char *buf = malloc(OS_EXEC_CAP + 512);
    if (!buf) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipefd[0]);
        if (exit_code) *exit_code = -1;
        return NULL;
    }

    size_t len = 0;
    bool timed_out = false;
    bool truncated = false;
    int64_t start = os_time_ms();
    int status = 0;

    while (1) {
        int remaining = timeout_ms > 0
            ? timeout_ms - (int)(os_time_ms() - start) : 1000;
        if (remaining <= 0) { timed_out = true; break; }
        if (remaining > 1000) remaining = 1000;

        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int pr = poll(&pfd, 1, remaining);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) { timed_out = true; break; }

        if (len >= OS_EXEC_CAP) { truncated = true; break; }

        ssize_t n = read(pipefd[0], buf + len, OS_EXEC_CAP - len);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break; /* EOF */
        len += (size_t)n;
    }

    buf[len] = '\0';
    close(pipefd[0]);

    if (timed_out || truncated) {
        /* Kill the command tree and don't wait for it to finish. */
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
        if (exit_code) *exit_code = timed_out ? 124 : -1;
        if (timed_out) {
            int olen = snprintf(buf + len, 256,
                "\n... (command timed out after %d ms)\n", timeout_ms);
            len += olen > 0 ? (size_t)olen : 0;
            buf[len] = '\0';
        } else {
            int olen = snprintf(buf + len, 256,
                "\n... (output truncated at %d bytes)\n", OS_EXEC_CAP);
            len += olen > 0 ? (size_t)olen : 0;
            buf[len] = '\0';
        }
        return buf;
    }

    /* Reap the child. If it is still alive after closing stdout, wait
     * briefly and kill it rather than blocking forever. */
    int waited = 0;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (timeout_ms > 0 && waited >= timeout_ms) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            if (exit_code) *exit_code = 124;
            return buf;
        }
        usleep(50000);
        waited += 50;
    }

    if (exit_code) {
#ifdef PLATFORM_WINDOWS
        *exit_code = status;
#else
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }
    return buf;
}

#endif /* !PLATFORM_WINDOWS */

char *os_exec_capture(const char *command, int *exit_code) {
#ifdef PLATFORM_WINDOWS
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
#else
    return os_exec_capture_timeout(command, 60000, exit_code);
#endif
}

/* ── Time ───────────────────────────────────────────────────────── */

int64_t os_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
