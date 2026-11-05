# Storage and modules

```lua
local counter = vesta.storage.get("counter") or 0
vesta.storage.set("counter", counter + 1)
vesta.storage.remove("counter")
```

Values are stored in a private JSON file selected by the package `id`, up to
1024 keys and 1 MiB in total; `set` returns `false` when a value would exceed
that.

`require` searches for `.lua` modules in the active package directory first,
then in the shared `lua/modules` directory. Several scripts can therefore share
the same source module without duplicating it.
