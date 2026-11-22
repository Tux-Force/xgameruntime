/* The Wine-only helpers our C modules use, for upstream's MinGW build. */
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
