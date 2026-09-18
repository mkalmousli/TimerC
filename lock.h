/* lock.h — hardware input lock ("-l" switch)
 *
 * Deauthorizes the USB HID devices behind the keyboard/mouse at the kernel
 * level (writes 0 to /sys/bus/usb/devices/<dev>/authorized), so the OS
 * itself stops seeing them — not just this app's X11 grab. Requires root.
 *
 * A detached watchdog process is forked off before the deauthorize happens.
 * It re-authorizes the devices either when told to (hw_lock_release) or
 * unconditionally after a timeout, so a crash of the main process (or of
 * the whole X session) can never leave the machine's own input dead.
 */
#ifndef LOCK_H
#define LOCK_H

#include <sys/types.h>

#define MAX_LOCK_DEVICES 16

/* Setuid-root helper installed by `sudo make install-lock-helper`. When
 * not running as root, hw_lock_engage() shells out to this instead of
 * writing sysfs directly, so the timer binary itself never needs root. */
#define LOCK_HELPER_PATH "/usr/local/bin/timer-lock-helper"

typedef struct {
    char sysfs_path[512]; /* .../authorized */
} LockDevice;

typedef struct {
    LockDevice devices[MAX_LOCK_DEVICES];
    int        n_devices;
    pid_t      watchdog_pid;
    int        active;
} HwLock;

/* Finds the USB keyboard/pointer devices, deauthorizes them, and spawns the
 * watchdog (which will re-authorize after timeout_secs+margin regardless,
 * or immediately on hw_lock_release()). Returns 1 if at least one device
 * was locked, 0 if not (not root, or no matching USB HID devices found —
 * caller should fall back to a softer lock in that case). */
int hw_lock_engage(HwLock *hl, int timeout_secs);

/* Signals the watchdog to re-authorize the devices now and waits briefly
 * for it to confirm. Safe to call even when not engaged. */
void hw_lock_release(HwLock *hl);

#endif
