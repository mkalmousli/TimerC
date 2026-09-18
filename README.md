# timer_c — Native Linux X11 Countdown Timer

A fully native C rewrite of the Pomodoro timer, using pure X11/Xlib with XFT
antialiased fonts. No GTK, no Qt, no Python — just X11, math, and POSIX.

## Build

```bash
make
make install   # copies to ~/.local/bin/timer, installs .desktop entry + icon
```

**Dependencies:** `libX11`, `libXft`, `libXrandr`, `libsqlite3`, `libm`, `libpthread`

```bash
sudo apt install libx11-dev libxft-dev libxrandr-dev libsqlite3-dev
```

## Usage

```bash
./timer                    # interactive config window
./timer 25m focus          # 25 minute timer with task "focus"
./timer 1h deep work       # 1 hour timer
./timer 30                 # 30 minutes (default unit)
./timer 1:30:00 coding     # 1h 30m via colon format
./timer u16:30 meeting     # timer until 16:30
./timer u1630              # same, compact form
./timer d30m shutdown now  # shutdown mode, 30 min
./timer p25m               # pausable, 25 min
./timer f1h fullscreen     # fullscreen mode, 1 hour
./timer l25m focus         # lock keyboard/mouse, 25 min
```

## Input Switches (prefix before time)

| Switch | Effect         |
|--------|----------------|
| `d`    | Shutdown mode  |
| `a`    | Alert mode     |
| `f`/`r`| Fullscreen     |
| `p`    | Pausable       |
| `l`    | Disconnect keyboard/mouse for the duration |

Example: `dp 30m` → shutdown + pausable, 30 minutes.

`l` disconnects your USB keyboard and mouse at the kernel level while the
timer counts down — not just an X11 grab. It deauthorizes the USB HID
device(s) behind them (`/sys/bus/usb/devices/*/authorized`), the same
mechanism `usbguard`/`authbind` use, so the OS itself stops seeing them;
they go dark system-wide, not just inside this app.

This needs root to flip that sysfs attribute, but `timer` itself never
runs as root. Instead there's a tiny setuid-root helper
(`timer-lock-helper`, built from `lockhelper.c`) that does nothing but
validate a path looks like `/sys/bus/usb/devices/<addr>/authorized` and
write `0`/`1` to it — install it **once**:

```sh
make install-lock-helper   # will prompt for sudo — installs to
                            # /usr/local/bin/timer-lock-helper, setuid root
```

After that, `timer l25m focus` run as your normal user (no `sudo`) fully
disconnects the devices. `make install` builds/installs everything else
without root and reminds you if the helper isn't in place yet; without
it, `-l` silently falls back to a plain X11 grab (still usable, but not a
real disconnect) and logs a warning. `sudo make uninstall-lock-helper`
removes it again.

While locked there is no way to cancel the countdown from the keyboard or
mouse — they're not attached to the machine. Middle-click stop, the
window-manager close button, and `SIGINT`/`SIGTERM` are also ignored for
good measure. **The only way out is `SIGKILL` or a hard reboot** — no
process can block those, which is also the safety net: a background
watchdog process (independent of the main one) re-authorizes the devices
the moment the countdown's time is up, or immediately if the main process
dies for any reason, so a crash can never leave your input permanently
dead. Once the timer actually finishes, control is handed back
immediately for the alert/shutdown that follows.

Only USB HID devices are covered (keyboards/mice on PS/2, Bluetooth, or a
laptop's internal I2C touchpad aren't). Check the log for which devices
were found and locked.

## CLI Flags

| Flag              | Description                    |
|-------------------|--------------------------------|
| `-m alert\|shutdown` | Mode                        |
| `-p / --pausable` | Enable pause/resume            |
| `-f / --fullscreen` | Start in fullscreen mode     |
| `-l / --lock`     | Disconnect keyboard/mouse for the duration |
| `--theme NAME`    | Theme by name (see below — names are generated fresh each run, so check the log/config window for this run's names) |
| `-h / --help`     | Show help                      |

## Controls (timer window)

| Action             | Effect                   |
|--------------------|--------------------------|
| Right-click        | Toggle mini ↔ fullscreen |
| Middle-click       | Stop timer               |
| Click (if pausable)| Pause / resume           |
| Drag               | Move mini window         |
| Drag ◢ corner      | Resize mini window       |
| ESC / Space        | Stop / pause             |
| Ctrl+T (config)    | Cycle theme              |

## Features

- **Procedurally generated black themes**: every launch builds 4 fresh near-black palettes — hues spread evenly (golden-angle steps) and each accent color's lightness is solved against the background for legible contrast (WCAG-style), so themes vary run to run but always stay coherent. Pin one with `startup.theme` in the config file if you want a fixed slot.  
- **Adaptive font sizing**: countdown fills the available space  
- **Color transition**: accent → danger as time runs out  
- **Animated progress bar**  
- **Glass chime audio cues**: start, pause, resume, half-time, last 3s, dismiss  
- **Alarm chime**: cheerful bell-arpeggio alert (ramping volume, configurable), strobing fullscreen alert  
- **Until-time**: `u15:30` counts down to a wall-clock time  
- **Shutdown mode**: calls `systemctl poweroff` when done  
- **Multi-monitor**: timer appears on the same monitor as the config window  
- **Session logging**: SQLite database + plain log file in `~/.local/share/timer_c/`

## Configuration

There is no config file. Alarm behavior, window sizes, redraw pacing,
and sleep-mode audio levels are plain values assigned directly to
`g_config` fields in `config_load()` in `config.c`, each documented
right there with its type/range/default/why. To change a value, edit
it and rebuild.

The `config/` directory next to the binary is unrelated runtime state
(session log and SQLite database) — not a settings file. Sleep-mode
audio (preparing/sleeping/waking) is entirely procedurally generated —
there are no custom audio files to drop in.

**One volume model:** every `*_volume_percent` field in `g_config`
(`alarm_volume_min/max_percent`, `sleep_volume_preparing/sleeping/
waking_percent`) is your actual **system/speaker output level**, 0-100%,
with 100% meaning full system volume — there is no separate app-level
audio gain layered on top. `set_system_volume()` in `audio.c` is the
*only* thing in the whole program that ever touches your system volume;
it's called the instant each sound (the alarm, or a sleep-mode phase)
starts, and re-asserted periodically while that sound plays, moving your
actual volume up or down — instantly, automatically — to match that
sound's configured percent, even if something else nudges the system
mixer in the meantime. Try it with `timer test
preparing|sleeping|waking|alarm|all` (see `--help`), which sets your
system volume exactly the same way before playing each phase's
generated audio directly (no external player).

See `config.c` for the full list of options.

## Audio Requirements (any one)

- `aplay` (ALSA, usually pre-installed)
- `pacat` (PulseAudio)
- `pw-play` (PipeWire)
- `ffplay` (FFmpeg)
- `mpv`
