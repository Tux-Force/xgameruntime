# tools

## gdkgen.py

Code generation and stub bookkeeping for xgameruntime. Needs Python 3.11 or later and nothing else.

Its only inputs are this repository's IDL (`include/`), C modules (`src/Modules/`) and Wine logs of real games. It never reads Microsoft
binaries and does not decompile anything, so the generated code stays as clean as the rest of the repository.

| Command | What it does |
|---|---|
| `tools/gdkgen.py report [LOG...]` | Markdown table of all stubs, by kind (register, async, result, ...). With Wine logs of game runs (`WINEDEBUG=fixme+gdkc`) it also counts how often each stub was hit, so the ones games really call come first. |
| `tools/gdkgen.py check` | Compares every C vtable with its IDL interface: order, names, entries missing at the end. A wrong slot makes a game call the wrong function, so the CI runs this. |
| `tools/gdkgen.py stubs [--write]` | Generates C stubs (FIXME trace, default return) for IDL methods that the vtable does not have yet, for example after a new interface version was added to the IDL. Prints them, or inserts the functions and vtable entries with `--write`. |
| `tools/gdkgen.py events [--write]` | Generates the event registrations listed in `gdkgen/events.toml`. Exits 1 when the code is out of date; `--write` updates it. |

### Event registrations that never fire

Many GDK events cannot happen under xTux, such as game invites from the Xbox shell or a package installed while the
game runs. For those, a stub returning `E_NOTIMPL` only makes games fail at start-up. `gdkgen/events.toml` lists
them with a reason. For each one, `events --write` generates a registration that checks its arguments, returns a
unique token and never invokes the callback, plus an unregistration that returns `TRUE`. The reason ends up as a
comment in the code, and a marker comment shows that the body is generated.

To change such a function, edit `events.toml` and run `events --write`; edits to a generated body are overwritten.
To implement the event for real, remove it from `events.toml` and replace the generated body by hand. The generator
leaves hand-written functions (no marker, no `stub!`) alone.

### Typical loop

```sh
WINEDEBUG=fixme+gdkc xtux-cli run <game> 2> game.log
tools/gdkgen.py report game.log | head -30
```

The top of the report shows the stubs to work on next. Events go into `events.toml`; everything else is written by
hand from the public GDK documentation.

## migrate_upstream.py (history)

This script moved our C modules onto upstream's C++ tree: it copied them as C into `src/Modules`, added
`wine_compat.h`, the `QueryApiImpl` dispatcher and the CMake lines, and rewrote Wine's statically initialised critical
sections. The move is done, so the script is gone from the tree; it is in the history of the
`feature/upstream-migrator` branch.
