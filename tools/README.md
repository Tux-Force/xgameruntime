# tools

## gdkgen.py

Code generation and stub bookkeeping for xgameruntime. Needs Python 3.11 or later and nothing else.

Its only inputs are this repository's `.idl` and `.c` files and Wine logs of real games. It never reads Microsoft
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

## migrate_upstream.py (prototype)

Moves C modules of this fork onto upstream's C++ rewrite ([xodus-gaming/xgameruntime](https://github.com/xodus-gaming/xgameruntime),
branch `main`). COM vtables are the same in C and C++, so the modules are not translated: they are copied as C files
into upstream's `src/Legacy/` and compiled there behind a small layer for the Wine-only things they use.

```sh
git clone --recurse-submodules https://github.com/xodus-gaming/xgameruntime upstream
tools/migrate_upstream.py upstream xgameprotocol xnetworking xstore xuser xpackage --wine-src ../wine
cmake --preset win64 -S upstream && cmake --build upstream/build/windows-x64
```

What it does, per run (always pass the complete list of modules to move):

- compares each module's IDL interfaces with upstream's and copies ours where they differ;
- follows what the modules need: helper files such as `xgameconfig.c` and `xodusbridge.c` come along, other
  interfaces must be moved in the same run, and anything from our `xthreading.c` is a blocker because upstream has
  its own XThreading (`--dry-run` lists the blockers without writing anything);
- rewrites the Wine-only idioms that do not compile against MinGW, for now the statically initialised
  `CRITICAL_SECTION_DEBUG` critical sections;
- writes `src/Legacy/` (modules, `private.h` built from ours, `wine_compat.h`, Wine's `wine/list.h`, a dispatcher for
  `QueryApiImpl`) and patches upstream's `src/main.cpp` and `src/CMakeLists.txt` between marker comments;
- links the import libraries from our `Makefile.in`.

`migrate_smoke.c` checks the result under Wine: it loads the built DLL, gets the moved modules through upstream's
`QueryApiImpl`, and has `XNetworking` deliver its connectivity hint through upstream's C++ task queue. Build and run
instructions are at the top of the file.

Not covered yet: modules that use `gdkc_async_context` from our XThreading (XGameSave), and building upstream's DLL
inside Tux-Force/wine instead of Wine's own `dlls/xgameruntime`.
