<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://tux-force.github.io/brand/logo-horizontal-dark-bg.png" />
    <img src="https://tux-force.github.io/brand/logo-horizontal-light-bg.png" alt="Tux-Force" width="420" />
  </picture>
</p>

# xgameruntime (Tux-Force)

Open source implementation of `xgameruntime.dll`, the Microsoft GDK runtime that Xbox PC games load, for use in Wine
with [xTux](https://github.com/Tux-Force/xTux). Part of [Tux-Force](https://github.com/Tux-Force); guides and
the compatibility list are on the website, **https://tux-force.github.io**.

This is a **soft fork of [xodus-gaming/xgameruntime](https://github.com/xodus-gaming/xgameruntime)**, built on
upstream's C++ code base (`upstream/main`): its CMake/MinGW build, its XTaskQueue/XAsync and its tests. Our modules,
written in C for upstream's earlier implementation (`upstream/legacy-c`), live in `src/Modules` and are compiled as C
into the same DLL. COM vtables are the same in C and C++, so `QueryApiImpl` hands them out next to upstream's own
classes.

> [!CAUTION]
> Unofficial community project, not affiliated with, endorsed by, or sponsored by Microsoft or Xbox. Our modules
> are written from public GDK documentation and observed behaviour and contain no Microsoft code. Upstream's task
> queue and async code (`src/System/Threading`) is adapted from Microsoft's open source, MIT licensed
> [libHttpClient](https://github.com/microsoft/libHttpClient); nothing comes from the closed Windows runtime.

## What this fork implements

- **XTaskQueue / XAsync:** upstream's implementation (`src/System`), based on Microsoft's open source libHttpClient.
- **XUser:** the signed-in user from xTux (XUID, gamertags, age group, privileges) and Xbox Live tokens with request
  signatures through the xTux bridge.
- **XStore:** game license state, store license tokens through the bridge, empty add-on queries, no-op package updates.
- **XGameSave / XPersistentLocalStorage:** file based containers and blobs in the game's package folder.
- **XNetworking:** connectivity hints (including the initial notification XCurl waits for), security information for
  URLs, preferred UDP port.
- **XPackage / XGame:** identity from `MicrosoftGame.config` (title id, store id, package family name, MSA app id).

## Generated code

`tools/gdkgen.py` checks the C vtables against the IDL, generates stubs for new interface versions and the event
registrations that can never fire under xTux, and reports which stubs games really hit. See
[tools/README.md](tools/README.md).

## Talking to xTux

`xtux-cli run` passes `XODUS_BRIDGE_PORT` and `XODUS_BRIDGE_SECRET`. `src/Modules/xodusbridge.c` sends plain text requests over
WinHTTP to `127.0.0.1`; the protocol is documented in xTux (`crates/xtux-games/src/bridge.rs`) and on the
website ([architecture](https://tux-force.github.io/project/architecture/#the-game-bridge)). Account
tokens never enter the game process, only the per-request results do.

## Layout

| Path | What |
|---|---|
| `include/` | IDL and headers of the GDK interfaces (widl generates the C/C++ headers) |
| `src/main.cpp`, `src/System/` | upstream: DLL entry, `QueryApiImpl`, XThreading |
| `src/Modules/` | our C modules, `modules_dispatch.c` (their part of `QueryApiImpl`) and `wine_compat.h` for the few Wine helpers they use (`wine/list.h`, `debugstr_a/w`) |
| `libs/` | upstream: static helper libraries |
| `tests/` | upstream's gtest suite and `tests/Modules/test_modules.c` for our modules |
| `tools/` | `gdkgen.py`, see below |

## Building

The DLL is a PE DLL built with MinGW and CMake (the `widl` from MinGW or Wine has to be in `PATH`):

```bash
git submodule update --init
cmake --preset win64
cmake --build build/windows-x64
```

`build/windows-x64/bin/xgameruntime.dll` is the result. Wine loads it as a native DLL from the game's prefix
(`system32`) with `WINEDLLOVERRIDES=xgameruntime=n`. Tests run under Wine on Linux:

```bash
cmake --preset win64-tests
cmake --build build/windows-x64
ctest --test-dir build/windows-x64 --output-on-failure
```

## License

LGPL-2.1-or-later, like upstream.
