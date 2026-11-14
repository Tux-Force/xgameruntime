# xgameruntime (Tux-Force)

Open source implementation of `xgameruntime.dll`, the Microsoft GDK runtime that Xbox PC games load, for use in Wine
with [xTux](https://github.com/Tux-Force/xTux). Part of [Tux-Force](https://github.com/Tux-Force).

This is a **soft fork of [xodus-gaming/xgameruntime](https://github.com/xodus-gaming/xgameruntime)**. The `tux-force`
branch builds on upstream's C implementation (`legacy-c`) and is used as the `dlls/xgameruntime` submodule of
[Tux-Force/wine](https://github.com/Tux-Force/wine). Upstream has started a C++ rewrite on `main`; porting our work
there is planned, module by module, with pull requests back to upstream.

> [!CAUTION]
> Unofficial community project, not affiliated with, endorsed by, or sponsored by Microsoft or XBOX. Written from
> public GDK documentation and observed behaviour; it contains no Microsoft code.

## What the `tux-force` branch implements

- **XTaskQueue / XAsync:** full task queues (manual, thread pool, serialized, immediate ports, delayed callbacks,
  monitors, waiters, termination) and async blocks with correct lifetimes.
- **XUser:** the signed-in user from xTux (XUID, gamertags, age group, privileges) and Xbox Live tokens with request
  signatures through the xTux bridge.
- **XStore:** game license state, store license tokens through the bridge, empty add-on queries, no-op package updates.
- **XGameSave / XPersistentLocalStorage:** file based containers and blobs in the game's package folder.
- **XNetworking:** connectivity hints (including the initial notification XCurl waits for), security information for
  URLs, preferred UDP port.
- **XPackage / XGame:** identity from `MicrosoftGame.config` (title id, store id, package family name, MSA app id).

## Talking to xTux

`xodus-cli run` passes `XODUS_BRIDGE_PORT` and `XODUS_BRIDGE_SECRET`. `xodusbridge.c` sends plain text requests over
WinHTTP to `127.0.0.1`; the protocol is documented in xTux (`crates/xodus-cli/src/commands/bridge.rs`). Account
tokens never enter the game process, only the per-request results do.

## Building

The DLL is built as part of the Wine tree:

```bash
make -j$(nproc) dlls/xgameruntime/all
```

## License

LGPL-2.1-or-later, like upstream.
