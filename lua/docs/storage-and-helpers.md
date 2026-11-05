# Storage и modules

```lua
local count = vesta.storage.get("count") or 0
vesta.storage.set("count", count + 1)
vesta.storage.remove("count")
```

Данные атомарно сохраняются в `%TEMP%\vesta\lua\data\<script-id>\store.json`:
до 1024 ключей и 1 MiB всего; `set` возвращает `false`, если значение не помещается.

`require` ищет `.lua`-модули сначала в директории активного пакета, затем в общей
`lua/modules`. Это позволяет нескольким скриптам использовать общие модули без
копирования исходников.
