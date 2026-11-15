#!/usr/bin/env python3
"""migrate_upstream: move C modules of this fork onto upstream's C++ code base.

Upstream (xodus-gaming/xgameruntime, branch main) is a CMake/MinGW rewrite in C++ that so far only
implements XThreading. COM vtables are the same in C and C++, so our C modules do not need to be
translated: they are copied as C files into upstream's tree (src/Modules/) and compiled there behind
a small compatibility layer for the few Wine-only things they use (wine/list.h, debugstr_a/w, debug
channels). QueryApiImpl in upstream's main.cpp gets one call that dispatches to the moved modules.

    tools/migrate_upstream.py UPSTREAM_DIR MODULE... --wine-src WINE_DIR

MODULE is the C file name without .c (xgameprotocol, xnetworking, ...). The script

  1. compares each module's IDL interfaces with upstream's and copies ours where they differ,
  2. checks what the module needs from other files: helper files (only gdkc_* helpers, no
     interface) come along automatically; other interfaces must be moved in the same run; anything
     from our xthreading.c is a blocker, because upstream brings its own XThreading,
  3. copies the modules, writes src/Modules/{private.h,wine_compat.h,modules_dispatch.c,
     modules.h}, copies wine/list.h, and patches src/main.cpp and src/CMakeLists.txt.

Re-running it with more modules replaces the generated files, so the module list is always the
complete list of moved modules. It never touches files outside UPSTREAM_DIR/src and include/.
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gdkgen  # noqa: E402

ROOT = gdkgen.ROOT
INC_BEGIN, INC_END = "/* BEGIN migrate_upstream include */", "/* END migrate_upstream include */"
CALL_BEGIN, CALL_END = "/* BEGIN migrate_upstream dispatch */", "/* END migrate_upstream dispatch */"
CMAKE_BEGIN, CMAKE_END = "# BEGIN migrate_upstream", "# END migrate_upstream"


def interfaces_of(idl_dir: Path) -> dict:
    old = gdkgen.ROOT
    gdkgen.ROOT = idl_dir
    try:
        return gdkgen.parse_interfaces()
    finally:
        gdkgen.ROOT = old


def impl_pointer(path: Path) -> list[str]:
    return re.findall(r"^IX\w+ \*(x_\w+_impl) =", path.read_text(), re.M)


def defined_helpers(path: Path) -> set[str]:
    return set(re.findall(r"^[A-Za-z_][\w \*]*?\b(gdkc_\w+)\(", path.read_text(), re.M))


def used(path: Path, pattern: str) -> set[str]:
    text = re.sub(r"/\*.*?\*/", "", path.read_text(), flags=re.S)
    return set(re.findall(pattern, text))


def query_branches() -> dict[str, list[tuple[str, str]]]:
    """impl pointer -> [(CLSID name, interface name)] from our main.c QueryApiImpl."""
    out: dict[str, list[tuple[str, str]]] = {}
    text = (ROOT / "main.c").read_text()
    for m in re.finditer(r"IsEqualGUID\( clsid, &(CLSID_\w+) \)\)\s*return (IX\w+)_QueryInterface\( (x_\w+_impl),", text):
        out.setdefault(m.group(3), []).append((m.group(1), m.group(2)))
    return out


def plan(modules: list[str]) -> tuple[list[Path], list[Path], list[str]]:
    """(module files, helper files, blockers)."""
    files = {p.stem: p for p in gdkgen.c_files()}
    missing = [m for m in modules if m not in files]
    if missing:
        sys.exit(f"unknown module(s): {', '.join(missing)}")
    helpers_of = {p: defined_helpers(p) for p in files.values()}
    impls_of = {p: set(impl_pointer(p)) for p in files.values()}
    chosen = [files[m] for m in modules]
    provided = {i for p in chosen for i in impls_of[p]} | {"x_threading_impl"}
    helper_files: list[Path] = []
    blockers: list[str] = []
    todo = list(chosen)
    while todo:
        p = todo.pop()
        for h in sorted(used(p, r"\b(gdkc_\w+)\(") - helpers_of[p]):
            owner = next((q for q, hs in helpers_of.items() if h in hs), None)
            if owner is None:
                blockers.append(f"{p.name}: {h} is not defined anywhere")
            elif owner.stem == "xthreading":
                blockers.append(f"{p.name}: {h} comes from our xthreading.c, which upstream replaces")
            elif impls_of[owner] and owner not in chosen:
                blockers.append(f"{p.name}: {h} comes from {owner.name}; move that module too")
            elif not impls_of[owner] and owner not in helper_files and owner not in chosen:
                helper_files.append(owner)
                todo.append(owner)
        for i in sorted(used(p, r"\b(x_\w+_impl)\b") - provided):
            blockers.append(f"{p.name}: uses {i}; move its module too")
    return chosen, helper_files, blockers


def compare_idl(module: Path, upstream: Path, write: bool) -> list[str]:
    notes = []
    ours_idl = ROOT / f"{module.stem}.idl"
    theirs_idl = upstream / "include" / f"{module.stem}.idl"
    if not ours_idl.exists():
        return notes
    ours = interfaces_of(ROOT)
    theirs = interfaces_of(upstream / "include") if theirs_idl.exists() else {}
    for name in interfaces_of_file(ours_idl):
        a = [m[1] for m in ours[name].methods]
        b = [m[1] for m in theirs[name].methods] if name in theirs else None
        if a != b:
            notes.append(f"{ours_idl.name}: {name} differs from upstream ({'missing' if b is None else f'{len(b)} vs {len(a)} methods'})")
    if notes and write:
        shutil.copy2(ours_idl, theirs_idl)
        notes.append(f"copied our {ours_idl.name} over upstream's")
    return notes


def interfaces_of_file(path: Path) -> list[str]:
    text = re.sub(r"/\*.*?\*/", "", path.read_text(), flags=re.S)
    return [m["name"] for m in gdkgen.IFACE_RE.finditer(text)]


WINE_COMPAT = r"""/* Generated by tools/migrate_upstream.py: the Wine-only parts our C modules use. */
#ifndef LEGACY_WINE_COMPAT_H
#define LEGACY_WINE_COMPAT_H

#include <windows.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Upstream has no debug channels: everything goes through private/logging.h. */
#define WINE_DEFAULT_DEBUG_CHANNEL(ch) extern int compat_debug_channel_##ch

/* A few rotating buffers per thread, like Wine's debugstr helpers. */
static inline char *compat_debug_buffer( void )
{
    static __thread char buffers[4][512];
    static __thread unsigned int next;
    return buffers[next++ % 4];
}

static inline const char *debugstr_a( const char *s )
{
    char *buf;
    if (!s) return "(null)";
    if ((ULONG_PTR)s >> 16 == 0) { buf = compat_debug_buffer(); snprintf( buf, 512, "#%04x", (unsigned)(ULONG_PTR)s ); return buf; }
    buf = compat_debug_buffer();
    snprintf( buf, 512, "\"%.500s\"", s );
    return buf;
}

static inline const char *debugstr_w( const WCHAR *s )
{
    char *buf;
    int len;
    if (!s) return "(null)";
    if ((ULONG_PTR)s >> 16 == 0) { buf = compat_debug_buffer(); snprintf( buf, 512, "#%04x", (unsigned)(ULONG_PTR)s ); return buf; }
    buf = compat_debug_buffer();
    buf[0] = '"';
    len = WideCharToMultiByte( CP_UTF8, 0, s, -1, buf + 1, 500, NULL, NULL );
    if (len <= 0) len = 1;
    buf[len] = '"';
    buf[len + 1] = 0;
    return buf;
}

#endif
"""


CS_RE = re.compile(
    r"static CRITICAL_SECTION (?P<cs>\w+);\n"
    r"static CRITICAL_SECTION_DEBUG (?P<dbg>\w+) =\n\{.*?\n\};\n"
    r"static CRITICAL_SECTION (?P=cs) = \{ &(?P=dbg), -1, 0, 0, 0, 0 \};\n",
    re.S,
)


def port_source(text: str) -> tuple[str, list[str]]:
    """Rewrite Wine-only idioms that do not compile against MinGW's headers."""
    notes = []

    # Wine's statically initialised critical sections rely on Wine's CRITICAL_SECTION_DEBUG
    # layout. Initialise them from a constructor instead, which runs when the DLL loads.
    def cs(m):
        notes.append(f"critical section {m['cs']}: initialised in a constructor")
        return (
            f"static CRITICAL_SECTION {m['cs']};\n"
            f"static void __attribute__((constructor)) init_{m['cs']}( void )\n"
            "{\n"
            f"    InitializeCriticalSection( &{m['cs']} );\n"
            "}\n"
        )

    text = CS_RE.sub(cs, text)
    if "CRITICAL_SECTION_DEBUG" in text:
        notes.append("CRITICAL_SECTION_DEBUG left that the rewrite did not recognise")
    return text, notes


def legacy_private_h() -> str:
    text = (ROOT / "private.h").read_text()
    if "#include <wine/debug.h>" not in text:
        sys.exit("private.h: expected #include <wine/debug.h>")
    text = text.replace(
        "#include <wine/debug.h>",
        "#include <private/logging.h>\n#include \"wine_compat.h\"",
    )
    text = text.replace(
        "#define COBJMACROS",
        "/* Generated by tools/migrate_upstream.py from our private.h. */\n"
        "#ifndef __WINESRC__\n#define __WINESRC__ 1\n#endif\n"
        "/* Like Wine: typed inline wrappers and const vtables in the widl headers. */\n"
        "#define WIDL_C_INLINE_WRAPPERS\n#define CONST_VTABLE\n"
        "#define WIDL_EXPLICIT_AGGREGATE_RETURNS\n#define COBJMACROS",
        1,
    )
    return text


def dispatch_c(modules: list[Path]) -> str:
    branches = query_branches()
    lines = [
        "/* Generated by tools/migrate_upstream.py: QueryApiImpl for the moved C modules. */",
        "#include <initguid.h>",
        '#include "private.h"',
        '#include "modules.h"',
        "",
        "HRESULT modules_query_api( const GUID *clsid, const GUID *iid, void **out )",
        "{",
    ]
    for p in modules:
        for ptr in impl_pointer(p):
            for clsid, iface in branches.get(ptr, []):
                lines.append(f"    if (IsEqualGUID( clsid, &{clsid} ))")
                lines.append(f"        return {iface}_QueryInterface( {ptr}, iid, out );")
    lines += ["    return REGDB_E_CLASSNOTREG;", "}", ""]
    return "\n".join(lines)


MODULES_H = """/* Generated by tools/migrate_upstream.py. */
#ifndef LEGACY_MODULES_H
#define LEGACY_MODULES_H

#ifdef __cplusplus
extern "C" {
#endif

/* QueryApiImpl for the C modules in src/Modules; REGDB_E_CLASSNOTREG when none of them has clsid. */
HRESULT modules_query_api( const GUID *clsid, const GUID *iid, void **out );

#ifdef __cplusplus
}
#endif

#endif
"""


def patch_between(text: str, begin: str, end: str, body: str, anchor: str, before: bool) -> str:
    block = f"{begin}\n{body}{end}\n"
    if begin in text:
        return re.sub(re.escape(begin) + r".*?" + re.escape(end) + r"\n", lambda _: block, text, count=1, flags=re.S)
    if anchor not in text:
        sys.exit(f"anchor not found: {anchor!r}")
    i = text.index(anchor)
    if not before:
        i += len(anchor)
    return text[:i] + block + text[i:]


def patch_main(upstream: Path) -> None:
    path = upstream / "src" / "main.cpp"
    text = path.read_text()
    text = patch_between(text, INC_BEGIN, INC_END, '#include "Modules/modules.h"\n', '#ifdef __cplusplus\nextern "C" {', True)
    call = (
        "    {\n"
        "        HRESULT hr = modules_query_api( &clsid, &iid, out );\n"
        "        if (hr != REGDB_E_CLASSNOTREG) return hr;\n"
        "    }\n"
    )
    text = patch_between(text, "    " + CALL_BEGIN, "    " + CALL_END, call, "    FIXME( \"%s not implemented, returning E_NOINTERFACE", True)
    path.write_text(text)


# Wine import libraries whose MinGW counterpart has another name (WinRT activation lives in
# combase on Wine and in runtimeobject's import library on MinGW).
MINGW_LIBS = {"combase": "runtimeobject"}


def link_libraries() -> list[str]:
    """The IMPORTS of our Makefile.in, as MinGW import libraries."""
    m = re.search(r"^IMPORTS\s*=\s*(.*)$", (ROOT / "Makefile.in").read_text(), re.M)
    return [MINGW_LIBS.get(lib, lib) for lib in (m.group(1).split() if m else [])]


def patch_cmake(upstream: Path, sources: list[str]) -> None:
    path = upstream / "src" / "CMakeLists.txt"
    text = path.read_text()
    body = (
        "# C modules moved from Tux-Force/xgameruntime by tools/migrate_upstream.py\n"
        "target_sources(xgameruntime-impl PRIVATE\n"
        + "".join(f"        Modules/{s}\n" for s in sources)
        + "        Modules/modules_dispatch.c )\n"
        f"target_link_libraries( xgameruntime-impl PRIVATE {' '.join(link_libraries())} )\n"
    )
    text = patch_between(text, CMAKE_BEGIN, CMAKE_END, body, "target_link_libraries( xgameruntime-impl PRIVATE", True)
    path.write_text(text)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("upstream", type=Path)
    ap.add_argument("modules", nargs="+")
    ap.add_argument("--wine-src", type=Path, required=True, help="Wine source tree, for include/wine/list.h")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    upstream = a.upstream.resolve()
    if not (upstream / "src" / "main.cpp").exists():
        sys.exit(f"{upstream}: not an upstream xgameruntime checkout (src/main.cpp missing)")
    chosen, helpers, blockers = plan(a.modules)
    print("modules: " + ", ".join(p.stem for p in chosen))
    print("helper files: " + (", ".join(p.stem for p in helpers) or "none"))
    for b in blockers:
        print(f"BLOCKER {b}")
    notes = [n for p in chosen for n in compare_idl(p, upstream, write=not a.dry_run and not blockers)]
    for n in notes:
        print(f"IDL {n}")
    if blockers:
        print("nothing written")
        return 1
    if a.dry_run:
        return 0

    legacy = upstream / "src" / "Modules"
    if legacy.exists():
        shutil.rmtree(legacy)
    (legacy / "wine").mkdir(parents=True)
    shutil.copy2(a.wine_src / "include" / "wine" / "list.h", legacy / "wine" / "list.h")
    for p in chosen + helpers:
        text, port_notes = port_source(p.read_text())
        (legacy / p.name).write_text(text)
        for n in port_notes:
            print(f"PORT {p.name}: {n}")
    (legacy / "private.h").write_text(legacy_private_h())
    (legacy / "wine_compat.h").write_text(WINE_COMPAT)
    (legacy / "modules.h").write_text(MODULES_H)
    (legacy / "modules_dispatch.c").write_text(dispatch_c(chosen))
    patch_main(upstream)
    patch_cmake(upstream, [p.name for p in chosen + helpers])
    print(f"written to {legacy}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
