# Vesta Lua API 1.0

The Lua API lets local scripts consume Vesta's published game state, add menu
controls and overlay graphics, update configuration values, and use the input
API. Scripts receive snapshots through the existing runtime and do not need to
know CS2 addresses or create additional memory-sampling loops.

## Installation

On first launch Vesta creates `%TEMP%\vesta\lua`:

```text
lua/
  scripts/     -- individual .lua files or packages
  modules/     -- shared Lua source modules
  data/        -- private JSON storage for scripts
  logs/
```

Scripts are managed in `Misc -> Lua API`: a list with on/off, settings, reload
and delete (to the Recycle Bin), plus `Import` for a `.lua` file and
`Open folder` for `scripts`. Import copies the file atomically and refreshes the
runtime at once.

## Sandbox

Every script runs in a sandbox and sees only the Vesta API and the safe part of
the standard library:

- `base` without `dofile` and `loadfile`; `load` accepts text only (bytecode is refused);
- `string` without `dump`, `table`, `math`, `utf8`, `coroutine`;
- `os` with `clock`, `time`, `date` and `difftime` only;
- `require` loads `.lua` only from the script's own folder and the shared `lua/modules`;
- `setmetatable` refuses finalizers (`__gc`);
- no `io`, `package`, `debug` or `vesta.helpers`.

Scripts keep their data with `vesta.storage`. The one exception is the bundled
web radar (`lua/scripts/Vesta Web Radar.lua` in the repository): Vesta recognises it
by the SHA-256 of its text (CRLF normalised to LF) compiled into the build, and
only that file gets full access (`io`, `os`, starting its PowerShell helper).
Any change to the radar file needs a new Vesta build, otherwise it runs sandboxed.

## Minimal example

```lua
vesta.ui.toggle("enabled", "Player counter", true)

vesta.events.on("frame", function(frame)
    if not vesta.ui.get("enabled") then return end
    vesta.draw.text(30, 90, "Players: " .. #frame.players, 255, 255, 255, 255)
end)
```

Continue with [runtime and lifecycle](runtime-and-lifecycle.md),
[game snapshots](game-snapshot.md), [events](events.md), [configuration](config.md),
[UI and drawing](ui-and-drawing.md), [input](input.md),
[storage and modules](storage-and-helpers.md), [runtime testing](testing.md),
[the Web Radar example](examples/web-radar.md), and the complete
[API reference](api-reference.md).
