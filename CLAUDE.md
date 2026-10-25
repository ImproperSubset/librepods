# librepods (personal fork)

The work here is the Qt Linux app in `linux/`, run daily on this machine. The
Android app and the rest of the tree are upstream's and stay untouched.

## Branches and remotes

- `origin` is upstream (kavishdevar/librepods); `fork` is ours
  (ImproperSubset/librepods). Push only to `fork`.
- `local/integration` is the branch that gets built and run. Fixes land on it
  directly, one commit per fix, with `linux:` subjects.
- Upstream no longer maintains the Qt app (it points to the Rust rewrite on
  `linux/rust`) and closes Qt PRs, so fixes are not sent upstream. Open
  upstream PRs against `linux/` are still worth reading as a source of fixes.

## Build, run, logs

- Build: `cmake --build linux/build -j"$(nproc)"` (configured Release).
- The running app is the systemd user unit `librepods.service`, which executes
  `linux/build/librepods --hide`. Rebuilding does not reload it:
  `systemctl --user restart librepods`.
- Logs: `journalctl --user -u librepods`. The audio side is in the
  `wireplumber` and `pipewire` user units.
- There is no test suite; a change is checked by building, restarting the unit
  and reading the journal for the behaviour it should produce.

## Code

- The app talks to PipeWire through libpulse (`linux/media/`), never through
  `pactl` or `wpctl`.
- `linux/playerstatuswatcher.cpp` is a stale copy that is not compiled; the
  built one is `linux/media/playerstatuswatcher.cpp`.
- Key material from the AirPods (Magic Cloud Keys: IRK, encryption key) must
  never be logged, at any level — the journal is readable beyond this app.
