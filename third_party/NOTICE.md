# Third-party dependencies

- **Lua 5.4.8** — official Lua source, MIT license.
  - Source: https://github.com/lua/lua (tag v5.4.8)
  - Trimmed to the interpreter/core plus safe base/table/string/math/utf8
    libraries.  `lua.c`, `luac.c`, `onelua.c`, `liolib.c`, `loslib.c`,
    `loadlib.c` and `ldblib.c` are omitted from this tree.
  - The runtime only opens base/table/string/math/utf8 and removes
    `load`, `loadfile`, `dofile`, `require`, `io`, `os`, `package` and `debug`
    from the sandbox.

- **nlohmann/json 3.11.3** — single-header JSON library, MIT license.
  - Source: https://github.com/nlohmann/json

No game assets, official Artemis binaries or proprietary engine code are
included in this repository.
