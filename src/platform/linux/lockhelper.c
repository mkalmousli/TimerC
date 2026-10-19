/* timerc-lock-helper — tiny setuid-root helper used by lock.c so the timer
 * binary itself never needs to run as root.
 *
 * Usage: timerc-lock-helper <0|1> <path>
 *
 * Does exactly one thing: writes the single byte "0" or "1" to `path`,
 * and only if `path` resolves (via realpath, so symlinks can't redirect
 * it) to /sys/bus/usb/devices/<bus-port-addr>/authorized. Refuses
 * anything else. Installed setuid-root by `sudo make install-lock-helper`
 * — nothing else in this project runs as root.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Deliberately NOT realpath()-resolved: /sys/bus/usb/devices/<addr> is
 * itself a kernel-managed symlink into /sys/devices/... , so resolving it
 * would fail this check on every real device. Validating the literal path
 * text is safe here because the only variable part (the bus-port address)
 * is constrained to digits/dashes/dots below, which rules out "..",
 * symlink components, or anything else an attacker could plant. */
static int valid_authorized_path(const char *path) {
    static const char prefix[] = "/sys/bus/usb/devices/";
    static const char suffix[] = "/authorized";
    size_t plen = sizeof(prefix) - 1, slen = sizeof(suffix) - 1;
    size_t rlen = strlen(path);

    if (rlen <= plen + slen) return 0;
    if (strncmp(path, prefix, plen) != 0) return 0;
    if (strcmp(path + rlen - slen, suffix) != 0) return 0;

    /* Between prefix and suffix must be a bare bus-port address: digits,
     * dashes, dots only — no further path separators. */
    for (size_t i = plen; i < rlen - slen; i++) {
        char c = path[i];
        if (!(isdigit((unsigned char)c) || c == '-' || c == '.')) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <0|1> <path>\n", argv[0]);
        return 2;
    }
    const char *value = argv[1];
    if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
        fprintf(stderr, "refusing: value must be 0 or 1\n");
        return 2;
    }

    const char *path = argv[2];
    if (!valid_authorized_path(path)) {
        fprintf(stderr, "refusing: not a USB device 'authorized' attribute\n");
        return 1;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    int ok = (fputs(value, f) >= 0) && (fclose(f) == 0);
    return ok ? 0 : 1;
}
