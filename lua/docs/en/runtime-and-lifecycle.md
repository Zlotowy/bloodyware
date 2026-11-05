# Runtime and lifecycle

Every active script receives its own `lua_State` and a background thread running
at `BELOW_NORMAL` priority. Lua is never called from the render thread. Drawing
commands are published through an immutable buffer and appear in the next
presentation frame.

## Lifecycle events

```lua
vesta.events.on("load", function() end)
vesta.events.on("tick", function(dt) end)       -- approximately 60 Hz
vesta.events.on("frame", function(snapshot) end)
vesta.events.on("unload", function() end)
```

`frame` uses latest-only semantics: a slow script never accumulates old frames.
Hot Reload watches the entry file and restarts only the selected script. Keys
and mouse buttons owned by the script are released on reload, stop, error, menu
open, loss of game input, and Vesta shutdown.

New source is compiled and executes its top level in a provisional VM first.
Config and input side effects remain disabled until the replacement succeeds;
`load` is delivered only after the atomic swap. If compilation fails, the active
VM continues running and the error appears in the menu. The old state's `unload`
callback runs exactly once.

Per-script limits are 64 MiB of Lua heap, 8192 draw commands, 512 UI controls,
256 event handlers and 8 ms for one Lua callback. Texts are cut to 1024 bytes,
control labels to 128 and control values to 4096; a select has up to 64 options.
A value passed to `config` or `storage` may hold up to 65536 elements and 1 MiB of
text. Exceeding a limit is reported in the Lua API tab.

Game frames are published at up to 60 Hz. Config snapshots update at up to 10 Hz
or immediately after a patch, but only after the active script uses
`vesta.config.*` or subscribes to `config_changed`. Player, item, projectile,
spectator, and bomb data is collected lazily according to the APIs and events a
script actually requests. With no active scripts, the Lua bridge is idle.

## Package manifest

Each package resides in its own directory:

```json
{
  "id": "example_stats",
  "name": "Example Stats",
  "version": "1.0.0",
  "author": "author",
  "description": "Local statistics",
  "api_version": 1,
  "entry": "main.lua"
}
```

`id` may contain Latin letters, digits, `_`, and `-`. Autoload and Hot Reload
are user preferences in Vesta and are not defined by the manifest.
