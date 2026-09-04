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
```

## Input Switches (prefix before time)

| Switch | Effect         |
|--------|----------------|
| `d`    | Shutdown mode  |
| `a`    | Alert mode     |
| `f`/`r`| Fullscreen     |
| `p`    | Pausable       |

Example: `dp 30m` → shutdown + pausable, 30 minutes.

## CLI Flags

| Flag              | Description                    |
|-------------------|--------------------------------|
| `-m alert\|shutdown` | Mode                        |
| `-p / --pausable` | Enable pause/resume            |
| `-f / --fullscreen` | Start in fullscreen mode     |
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

Alarm behavior, window sizes, and redraw pacing are tunable at
`~/.config/timer_c/config.conf` (or `$XDG_CONFIG_HOME/timer_c/config.conf`).
The file is created with every option documented — description, valid
range, default, and an example — the first time you run the app; edit it
and restart to apply changes. `config.example.conf` in this repo is a copy
for reference without having to run the app first.

Format is `key = value`, one per line; `#` starts a comment. Unknown keys
are logged and skipped; out-of-range values are clamped to the nearest
valid value and logged — the file is never rejected.

```conf
# Louder alarm, faster fade-in, always start in fullscreen
alarm.volume_percent = 70
alarm.ramp_seconds = 3
startup.fullscreen = 1
```

**System volume guarantee:** `alarm.volume_percent` is the *only* setting
in the whole program that ever touches your system/master output volume,
and it is only ever applied immediately before the alarm sound starts —
never on launch, for UI cues, or otherwise.

See `config.c` for the full list of options (also enumerated in the
generated config file).

## Audio Requirements (any one)

- `aplay` (ALSA, usually pre-installed)
- `pacat` (PulseAudio)
- `pw-play` (PipeWire)
- `ffplay` (FFmpeg)
- `mpv`
