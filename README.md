# timer_c — Native Linux X11 Countdown Timer

A fully native C rewrite of the Pomodoro timer, using pure X11/Xlib with XFT
antialiased fonts. No GTK, no Qt, no Python — just X11, math, and POSIX.

## Build

```bash
make
make install   # copies to ~/.local/bin/timer
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
| `--theme NAME`    | Theme: Ember/Moss/Tide/Ash/Plum|
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

- **5 dark themes**: Night Ember, Moss, Tide, Ash, Plum  
- **Adaptive font sizing**: countdown fills the available space  
- **Color transition**: accent → danger as time runs out  
- **Animated progress bar**  
- **Glass chime audio cues**: start, pause, resume, half-time, last 3s, dismiss  
- **Alarm siren**: ramping volume over 15s, strobing fullscreen alert  
- **Until-time**: `u15:30` counts down to a wall-clock time  
- **Shutdown mode**: calls `systemctl poweroff` when done  
- **Multi-monitor**: timer appears on the same monitor as the config window  
- **Session logging**: SQLite database + plain log file in `~/.local/share/timer_c/`

## Audio Requirements (any one)

- `aplay` (ALSA, usually pre-installed)
- `pacat` (PulseAudio)
- `pw-play` (PipeWire)
- `ffplay` (FFmpeg)
- `mpv`
