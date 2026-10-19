# Contributing

1. Fork, then `make` (see README for dependencies).
2. Keep the C style of the surrounding code; build must stay warning-clean for new code (`-Wall -Wextra`).
3. Test on a virtual display so nothing touches your desktop: `xvfb-run -a ./timerc 3s`.
4. Open a PR describing the change and why.

Bugs: please include distro, window manager, `timerc --version` and `~/.config/timerc/timer.log`.
