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

This is a **soft fork of [xodus-gaming/xgameruntime](https://github.com/xodus-gaming/xgameruntime)**. Our `develop`
and `main` branches build on upstream's C implementation (`upstream/legacy-c`) and are used as the
`dlls/xgameruntime` submodule of [Tux-Force/wine](https://github.com/Tux-Force/wine). Upstream has started a C++
rewrite (`upstream/main`); moving our work
onto that code base is planned, module by module.

> [!CAUTION]
> Unofficial community project, not affiliated with, endorsed by, or sponsored by Microsoft or Xbox. Written from
> public GDK documentation and observed behaviour; it contains no Microsoft code.

## What this fork implements

- **XTaskQueue / XAsync:** full task queues (manual, thread pool, serialized, immediate ports, delayed callbacks,
  monitors, waiters, termination) and async blocks with correct lifetimes.
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

`xtux-cli run` passes `XODUS_BRIDGE_PORT` and `XODUS_BRIDGE_SECRET`. `xodusbridge.c` sends plain text requests over
WinHTTP to `127.0.0.1`; the protocol is documented in xTux (`crates/xtux-games/src/bridge.rs`) and on the
website ([architecture](https://tux-force.github.io/project/architecture/#the-game-bridge)). Account
tokens never enter the game process, only the per-request results do.

## Building

The DLL is built as part of the Wine tree:

```bash
make -j$(nproc) dlls/xgameruntime/all
```

## License

LGPL-2.1-or-later, like upstream.
