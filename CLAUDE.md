# CLAUDE.md

Project guidance for working in this repo.

## Keep the command reference current

[`COMMANDS.md`](COMMANDS.md) is the copy-paste command reference for the headless
sand table. **Whenever a command, route, setting, or its behavior changes, update
`COMMANDS.md` in the same change** — add new `$Sand/*`, `$LED/*`, `$Playlist/*`,
`/sand_*` routes; remove deleted ones; fix any altered semantics (values, defaults,
encoding, gotchas). Treat it like docs that ship with the code, not an afterthought.

[`API.md`](API.md) is the stable contract + JSON schema; keep it in sync too when
the interface changes. `COMMANDS.md` is the practical cheatsheet; `API.md` is the
spec.
