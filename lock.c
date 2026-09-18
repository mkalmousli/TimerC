/* lock.c — hardware input lock: see lock.h */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "lock.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* True if the setuid-root helper is installed and actually setuid-root
 * (not just present) — a stale/tampered copy with the right name but
 * wrong owner/mode must never be trusted. */
static int helper_available(void) {
    struct stat st;
    if (stat(LOCK_HELPER_PATH, &st) != 0) return 0;
    return st.st_uid == 0 && (st.st_mode & S_ISUID) && (st.st_mode & S_IXUSR);
}

static int run_helper(const char *value, const char *path) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        execl(LOCK_HELPER_PATH, LOCK_HELPER_PATH, value, path, (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Can we actually flip 'authorized' one way or another — either we're
 * root outright, or the setuid helper is there to do it for us? */
static int can_write_authorized(void) {
    return geteuid() == 0 || helper_available();
}

/* "1-3", "1-3.2", "2-1.4.1" — a USB bus-port address: digits, at least one
 * '-', optional '.'-separated port hops. Never contains ':' (that marks an
 * interface subdirectory, e.g. "1-3:1.0", which is one level too deep —
 * authorized lives on the device, not the interface). */
static int looks_like_usb_addr(const char *s) {
    if (!isdigit((unsigned char)*s)) return 0;
    int seen_dash = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '-') seen_dash = 1;
        else if (*p == '.' || isdigit((unsigned char)*p)) { /* ok */ }
        else return 0;
    }
    return seen_dash;
}

static int extract_usb_device_id(const char *sysfs_field, char *out, size_t outsz) {
    char work[768];
    snprintf(work, sizeof(work), "%s", sysfs_field);
    out[0] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(work, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (looks_like_usb_addr(tok)) snprintf(out, outsz, "%s", tok);
    }
    return out[0] != '\0';
}

/* USB devices backing a sound card (headsets, speakerphones, mics with
 * inline media-key handlers) show up as "kbd" input handlers too — their
 * volume/mute buttons register as a keyboard device — so find_lock_devices
 * would otherwise deauthorize your headphones along with the real
 * keyboard/mouse. Cross-reference /sys/class/sound to exclude any USB
 * device that also owns an audio card. */
static void collect_audio_exclusions(char excl[][64], int *n_excl, int max) {
    DIR *d = opendir("/sys/class/sound");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && *n_excl < max) {
        if (strncmp(de->d_name, "card", 4) != 0) continue;
        char link[600];
        snprintf(link, sizeof(link), "/sys/class/sound/%s/device", de->d_name);
        char resolved[PATH_MAX];
        if (!realpath(link, resolved)) continue;
        char devid[64];
        if (!extract_usb_device_id(resolved, devid, sizeof(devid))) continue;
        int dup = 0;
        for (int k = 0; k < *n_excl; k++)
            if (!strcmp(excl[k], devid)) { dup = 1; break; }
        if (!dup) snprintf(excl[(*n_excl)++], 64, "%s", devid);
    }
    closedir(d);
}

static int is_keyboard_or_pointer_block(const char *block) {
    const char *h = strstr(block, "Handlers=");
    if (!h) return 0;
    return strstr(h, "kbd") != NULL || strstr(h, "mouse") != NULL;
}

/* Scans /proc/bus/input/devices for keyboard/mouse entries and resolves
 * each to its parent USB device's sysfs "authorized" attribute, de-duped
 * (a keyboard often exposes several HID interfaces on one USB device). */
static int find_lock_devices(HwLock *hl) {
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return 0;

    char excl[MAX_LOCK_DEVICES][64];
    int n_excl = 0;
    collect_audio_exclusions(excl, &n_excl, MAX_LOCK_DEVICES);

    char ids[MAX_LOCK_DEVICES][64];
    int n_ids = 0;
    char block[4096] = "";
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\n') {
            if (is_keyboard_or_pointer_block(block)) {
                const char *s = strstr(block, "Sysfs=");
                if (s) {
                    s += 6;
                    char field[512];
                    size_t i = 0;
                    while (s[i] && s[i] != '\n' && i < sizeof(field) - 1) { field[i] = s[i]; i++; }
                    field[i] = '\0';
                    char devid[64];
                    if (extract_usb_device_id(field, devid, sizeof(devid))) {
                        int skip = 0;
                        for (int k = 0; k < n_excl; k++)
                            if (!strcmp(excl[k], devid)) { skip = 1; break; }
                        for (int k = 0; !skip && k < n_ids; k++)
                            if (!strcmp(ids[k], devid)) { skip = 1; break; }
                        if (!skip && n_ids < MAX_LOCK_DEVICES)
                            snprintf(ids[n_ids++], sizeof(ids[0]), "%s", devid);
                    }
                }
            }
            block[0] = '\0';
        } else {
            strncat(block, line, sizeof(block) - strlen(block) - 1);
        }
    }
    fclose(f);

    /* Only check the file exists here — whether it's *writable* depends on
     * whether we end up going through the setuid helper (root regardless
     * of our own uid) or direct fopen (needs our uid to be root already),
     * which write_authorized() decides per-call. */
    for (int i = 0; i < n_ids && hl->n_devices < MAX_LOCK_DEVICES; i++) {
        char path[512];
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/authorized", ids[i]);
        if (access(path, F_OK) != 0) continue;
        snprintf(hl->devices[hl->n_devices].sysfs_path,
                 sizeof(hl->devices[0].sysfs_path), "%s", path);
        hl->n_devices++;
    }
    return hl->n_devices > 0;
}

/* Returns how many of hl's devices were actually flipped to `value'. */
static int write_authorized(const HwLock *hl, char value) {
    char val_str[2] = { value, '\0' };
    int n_ok = 0;
    int is_root = (geteuid() == 0);
    for (int i = 0; i < hl->n_devices; i++) {
        int ok = 0;
        if (is_root) {
            FILE *wf = fopen(hl->devices[i].sysfs_path, "w");
            if (wf) { ok = (fputc(value, wf) != EOF); fclose(wf); }
        } else {
            ok = run_helper(val_str, hl->devices[i].sysfs_path);
        }
        if (ok) n_ok++;
    }
    return n_ok;
}

/* Runs as the detached watchdog: blocks until SIGUSR1 (release requested)
 * or the deadline passes, then re-authorizes and exits. Never returns. */
static void watchdog_body(const HwLock *hl, int timeout_secs) {
    setsid();
    signal(SIGHUP,  SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT,  SIG_IGN);
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_secs + 10; /* safety margin past the countdown */

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain = deadline.tv_sec - now.tv_sec;
        if (remain <= 0) break;
        struct timespec ts = { remain > 5 ? 5 : remain, 0 };
        if (sigtimedwait(&set, NULL, &ts) == SIGUSR1) break;
    }

    write_authorized(hl, '1');
    _exit(0);
}

int hw_lock_engage(HwLock *hl, int timeout_secs) {
    memset(hl, 0, sizeof(*hl));
    if (!can_write_authorized()) return 0;
    if (!find_lock_devices(hl)) return 0;

    int n_deauthed = write_authorized(hl, '0');
    if (n_deauthed == 0) { hl->n_devices = 0; return 0; }

    pid_t pid = fork();
    if (pid < 0) {
        /* Couldn't get a watchdog — re-authorize immediately rather than
         * leave devices dead with nothing able to bring them back. */
        write_authorized(hl, '1');
        hl->n_devices = 0;
        return 0;
    }
    if (pid == 0) {
        watchdog_body(hl, timeout_secs);
        _exit(0); /* unreachable */
    }
    hl->watchdog_pid = pid;
    hl->active = 1;
    return 1;
}

void hw_lock_release(HwLock *hl) {
    if (!hl->active && hl->watchdog_pid <= 0) return;
    if (hl->watchdog_pid > 0) {
        kill(hl->watchdog_pid, SIGUSR1);
        for (int i = 0; i < 100; i++) {
            int status;
            pid_t r = waitpid(hl->watchdog_pid, &status, WNOHANG);
            if (r == hl->watchdog_pid) break;
            usleep(20000);
        }
        hl->watchdog_pid = -1;
    }
    hl->active = 0;
}
