/* uninstall.c — `timerc-uninstall` (or `timerc uninstall`).
 *
 * Removes what `make install` / install.sh put under <prefix>: the
 * binaries, man pages, desktop entry and icon. The prefix is derived from
 * where this binary actually runs, so it works for any PREFIX. Settings and
 * history are kept unless --purge is given. */
#include "timer.h"
#include <unistd.h>
#include <errno.h>
#include <libgen.h>

#define MAX_TARGETS 24

static const char *MAN_PAGES[] = { "timerc.1", "timerc-bg.1", "timerc-config.1", "timerc-insights.1", "timerc-uninstall.1" };

static void usage(FILE *out) {
    fprintf(out,
        "timerc-uninstall - remove TimerC from this system\n\n"
        "Usage: timerc-uninstall [-y] [-n] [--purge]\n"
        "       timerc uninstall [-y] [-n] [--purge]\n\n"
        "  -y, --yes      do not ask for confirmation\n"
        "  -n, --dry-run  only list what would be removed\n"
        "  --purge        also delete your settings, history and log (~/.config/timerc)\n"
        "  -h, --help     this help\n\n"
        "Removes the binaries, man pages, desktop entry and icon found next to this\n"
        "binary. Your data is kept unless --purge is given.\n");
}

static int exists(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0;
}

int uninstall_main(int argc, char **argv) {
    int yes = 0, dry = 0, purge = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-y") || !strcmp(a, "--yes")) yes = 1;
        else if (!strcmp(a, "-n") || !strcmp(a, "--dry-run")) dry = 1;
        else if (!strcmp(a, "--purge")) purge = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else { fprintf(stderr, "timerc-uninstall: unknown option '%s'\n\n", a); usage(stderr); return 2; }
    }

    char exe[MAX_PATH_LEN];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { fprintf(stderr, "timerc-uninstall: cannot locate the running binary\n"); return 1; }
    exe[n] = '\0';

    char tmp[MAX_PATH_LEN], bindir[MAX_PATH_LEN], prefix[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", exe);
    snprintf(bindir, sizeof(bindir), "%s", dirname(tmp));
    snprintf(tmp, sizeof(tmp), "%s", bindir);
    if (strcmp(basename(tmp), "bin") != 0) {
        fprintf(stderr, "timerc-uninstall: %s is not an installed location (not in a bin/ directory).\n"
                        "If you unpacked a release archive, just delete its folder.\n", bindir);
        return 1;
    }
    snprintf(tmp, sizeof(tmp), "%s", bindir);
    snprintf(prefix, sizeof(prefix), "%s", dirname(tmp));

    if (!strcmp(bindir, "/usr/bin") && exists("/var/lib/dpkg/info/timerc.list")) {
        fprintf(stderr, "TimerC was installed by the package manager. Remove it with:\n"
                        "  sudo apt remove timerc\n");
        return 1;
    }

    char targets[MAX_TARGETS][MAX_PATH_LEN];
    int nt = 0;
#define ADD(...) do { char p_[MAX_PATH_LEN]; \
        int r_ = snprintf(p_, sizeof(p_), __VA_ARGS__); \
        if (r_ > 0 && r_ < (int)sizeof(p_) && exists(p_) && nt < MAX_TARGETS) \
            memcpy(targets[nt++], p_, (size_t)r_ + 1); } while (0)
    ADD("%s/timerc", bindir);
    ADD("%s/timerc-bg", bindir);
    ADD("%s/timerc-uninstall", bindir);
    ADD("%s/timerc-lock-helper", bindir);
    if (strcmp(bindir, "/usr/local/bin")) ADD("/usr/local/bin/timerc-lock-helper");
    for (size_t i = 0; i < sizeof(MAN_PAGES) / sizeof(*MAN_PAGES); i++)
        ADD("%s/share/man/man1/%s", prefix, MAN_PAGES[i]);
    ADD("%s/share/applications/timerc.desktop", prefix);
    ADD("%s/share/icons/hicolor/scalable/apps/timerc.svg", prefix);

    char cfgdir[MAX_PATH_LEN] = "";
    if (purge) {
        get_config_dir(cfgdir, sizeof(cfgdir));
        ADD("%s/config.conf", cfgdir);
        ADD("%s/data.db", cfgdir);
        ADD("%s/data.db-journal", cfgdir);
        ADD("%s/timer.log", cfgdir);
    }
#undef ADD

    if (nt == 0) { printf("Nothing to remove.\n"); return 0; }
    printf("%s:\n", dry ? "Would remove" : "This will remove");
    for (int i = 0; i < nt; i++) printf("  %s\n", targets[i]);
    if (!purge) printf("Your settings and history in ~/.config/timerc are kept (use --purge to delete them).\n");
    if (dry) return 0;

    if (!yes) {
        if (!isatty(STDIN_FILENO)) { fprintf(stderr, "Not a terminal: pass -y to confirm.\n"); return 1; }
        printf("Continue? [y/N] ");
        fflush(stdout);
        char ans[16] = "";
        if (!fgets(ans, sizeof(ans), stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
            printf("Aborted.\n");
            return 1;
        }
    }

    int failed = 0, denied = 0;
    for (int i = 0; i < nt; i++) {
        if (unlink(targets[i]) != 0) {
            fprintf(stderr, "  cannot remove %s: %s\n", targets[i], strerror(errno));
            failed++;
            if (errno == EACCES || errno == EPERM || errno == EROFS) denied++;
        }
    }
    if (purge && cfgdir[0]) rmdir(cfgdir);   /* only succeeds when empty */

    if (failed) {
        if (denied) fprintf(stderr, "Some files need elevated rights; re-run with: sudo %s uninstall%s\n",
                            exe, purge ? " --purge" : "");
        return 1;
    }
    printf("TimerC removed.\n");
    return 0;
}
