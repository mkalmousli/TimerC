/* linux_fs.c — filesystem/path resolution (Linux backend for platform.h)
 *
 * Relocated from config.c: resolving where the app's "config" directory
 * (config.conf, session log, SQLite database) lives is OS-specific
 * (/proc/self/exe), not a config-*value* concern.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "platform.h"

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 4096
#endif

/* Per-user data dir: $XDG_CONFIG_HOME/timerc (default ~/.config/timerc),
 * created on demand. Holds config.conf, the SQLite database and the log. */
void get_config_dir(char *out, int out_sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char base[MAX_PATH_LEN], dir[MAX_PATH_LEN + 16];
    if (xdg && *xdg)       snprintf(base, sizeof(base), "%s", xdg);
    else if (home && *home) snprintf(base, sizeof(base), "%s/.config", home);
    else                    snprintf(base, sizeof(base), ".");
    mkdir(base, 0755);
    snprintf(dir, sizeof(dir), "%s/timerc", base);
    mkdir(dir, 0755);
    snprintf(out, out_sz, "%s", dir);
}
