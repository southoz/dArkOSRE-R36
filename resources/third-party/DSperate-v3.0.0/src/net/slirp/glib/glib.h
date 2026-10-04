/* SPDX-License-Identifier: BSD-3-Clause
 *
 * A stand-in for the small part of GLib that libslirp uses, so the vendored
 * copy in ../ builds with no dependency beyond libc. See ../README.md for
 * what is deliberately stubbed (the fork_exec path) and why that is safe.
 *
 * Not a GLib implementation: nothing outside libslirp should include this.
 */
#ifndef DSPERATE_GLIB_SHIM_H
#define DSPERATE_GLIB_SHIM_H

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- types ------------------------------------------------------------- */
typedef char gchar;
typedef unsigned char guchar;
typedef short gshort;
typedef unsigned short gushort;
typedef int gint;
typedef unsigned int guint;
typedef long glong;
typedef unsigned long gulong;
typedef int gboolean;
typedef float gfloat;
typedef double gdouble;
typedef void *gpointer;
typedef const void *gconstpointer;
typedef size_t gsize;
typedef ptrdiff_t gssize;

typedef int8_t gint8;
typedef uint8_t guint8;
typedef int16_t gint16;
typedef uint16_t guint16;
typedef int32_t gint32;
typedef uint32_t guint32;
typedef int64_t gint64;
typedef uint64_t guint64;

typedef gchar **GStrv;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define G_MAXUINT UINT_MAX
#define G_MAXINT INT_MAX

/* ---- compiler glue ----------------------------------------------------- */
#if defined(__GNUC__)
#define G_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define G_LIKELY(expr) __builtin_expect(!!(expr), 1)
#define G_GNUC_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#define G_GNUC_UNUSED __attribute__((unused))
#define G_GNUC_NORETURN __attribute__((noreturn))
#else
#define G_UNLIKELY(expr) (expr)
#define G_LIKELY(expr) (expr)
#define G_GNUC_PRINTF(fmt_idx, arg_idx)
#define G_GNUC_UNUSED
#define G_GNUC_NORETURN
#endif

/* GLib's MIN/MAX are function-like macros, not functions; libslirp calls
 * them with side-effect-free arguments only. */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define CLAMP(x, low, high) MIN(MAX((x), (low)), (high))

#define G_N_ELEMENTS(arr) (sizeof(arr) / sizeof((arr)[0]))
#define G_SIZEOF_MEMBER(type, member) sizeof(((type *)0)->member)
#define G_STATIC_ASSERT(expr) _Static_assert((expr), #expr)
#define G_BEGIN_DECLS
#define G_END_DECLS

/* libslirp only ever asks for >= 2.58 (the g_spawn_async_with_fds guard in
 * misc.c); answering yes keeps it on the single-call path. */
#define GLIB_CHECK_VERSION(major, minor, micro) \
    ((major) < 2 || ((major) == 2 && (minor) <= 58))

/* Not decoration: libslirp branches on this in two places that change
 * behaviour and layout, both silently if it is missing. cksum.c picks its
 * 64-bit accumulator loop, and ip.h pads `struct mbuf_ptr` to eight bytes on
 * a 32-bit target so the overlay over the IP header still lines up. Left
 * undefined it compiles clean, passes the link, and drops every IP packet on
 * a 32-bit build -- which is how it was found (the A30, glibc 2.23 armv7l). */
#define GLIB_SIZEOF_VOID_P __SIZEOF_POINTER__

#define G_LITTLE_ENDIAN 1234
#define G_BIG_ENDIAN 4321
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define G_BYTE_ORDER G_BIG_ENDIAN
#else
#define G_BYTE_ORDER G_LITTLE_ENDIAN
#endif

#if !defined(_WIN32)
#define G_OS_UNIX 1
#endif

/* ---- memory ------------------------------------------------------------ */
/* GLib aborts on allocation failure and libslirp is written to that
 * contract (it never checks a g_malloc result), so the shim aborts too
 * rather than handing back a NULL nothing tests for. */
void *slirp_g_malloc(gsize n_bytes, gboolean zeroed);
void *slirp_g_realloc(void *mem, gsize n_bytes);

#define g_malloc(sz) slirp_g_malloc((sz), FALSE)
#define g_malloc0(sz) slirp_g_malloc((sz), TRUE)
#define g_realloc(p, sz) slirp_g_realloc((p), (sz))
#define g_free(p) free(p)
#define g_new(type, n) ((type *)slirp_g_malloc(sizeof(type) * (gsize)(n), FALSE))
#define g_new0(type, n) ((type *)slirp_g_malloc(sizeof(type) * (gsize)(n), TRUE))

/* ---- strings ----------------------------------------------------------- */
gchar *g_strdup(const gchar *str);
const gchar *g_strerror(gint errnum);
gboolean g_str_has_prefix(const gchar *str, const gchar *prefix);
gint g_ascii_strcasecmp(const gchar *s1, const gchar *s2);
gchar *g_strstr_len(const gchar *haystack, gssize haystack_len, const gchar *needle);
gsize g_strlcpy(gchar *dest, const gchar *src, gsize dest_size);
guint g_strv_length(gchar **str_array);
void g_strfreev(gchar **str_array);
const gchar *g_getenv(const gchar *variable);

#define g_vsnprintf vsnprintf
#define g_snprintf snprintf

/* GString: libslirp uses it only to build the connection/neighbour info
 * strings, so this is an append-only buffer and nothing more. */
typedef struct GString {
    gchar *str;
    gsize len;
    gsize allocated_len;
} GString;

GString *g_string_new(const gchar *init);
void g_string_append_printf(GString *string, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
gchar *g_string_free(GString *string, gboolean free_segment);

/* ---- diagnostics ------------------------------------------------------- */
void slirp_g_log(const char *level, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void slirp_g_abort(const char *fmt, ...) G_GNUC_PRINTF(1, 2) G_GNUC_NORETURN;

#define g_debug(...) slirp_g_log("DEBUG", __VA_ARGS__)
#define g_warning(...) slirp_g_log("WARNING", __VA_ARGS__)
#define g_critical(...) slirp_g_log("CRITICAL", __VA_ARGS__)
#define g_error(...) slirp_g_abort(__VA_ARGS__)

#define g_warning_once(...)                        \
    do {                                           \
        static gboolean slirp_warned_ = FALSE;     \
        if (!slirp_warned_) {                      \
            slirp_warned_ = TRUE;                  \
            slirp_g_log("WARNING", __VA_ARGS__);   \
        }                                          \
    } while (0)

#define g_assert(expr)                                                     \
    do {                                                                   \
        if (G_UNLIKELY(!(expr)))                                           \
            slirp_g_abort("assertion failed: %s (%s:%d)", #expr,           \
                          __FILE__, __LINE__);                             \
    } while (0)
#define g_assert_not_reached() \
    slirp_g_abort("code should not be reached (%s:%d)", __FILE__, __LINE__)

#define g_return_if_fail(expr)                                             \
    do {                                                                   \
        if (G_UNLIKELY(!(expr))) {                                         \
            slirp_g_log("CRITICAL", "%s: assertion '%s' failed",           \
                        __func__, #expr);                                  \
            return;                                                        \
        }                                                                  \
    } while (0)
#define g_return_val_if_fail(expr, val)                                    \
    do {                                                                   \
        if (G_UNLIKELY(!(expr))) {                                         \
            slirp_g_log("CRITICAL", "%s: assertion '%s' failed",           \
                        __func__, #expr);                                  \
            return (val);                                                  \
        }                                                                  \
    } while (0)
#define g_warn_if_fail(expr)                                               \
    do {                                                                   \
        if (G_UNLIKELY(!(expr)))                                           \
            slirp_g_log("WARNING", "%s: check '%s' failed", __func__,      \
                        #expr);                                            \
    } while (0)
#define g_warn_if_reached() \
    slirp_g_log("WARNING", "%s: code should not be reached", __func__)

typedef struct GDebugKey {
    const gchar *key;
    guint value;
} GDebugKey;
guint g_parse_debug_string(const gchar *string, const GDebugKey *keys, guint nkeys);

/* ---- random ------------------------------------------------------------ */
/* libslirp wants unpredictable ISNs and ephemeral ports, not reproducible
 * ones, so this is seeded from the clock and the pid. */
typedef struct GRand GRand;
GRand *g_rand_new(void);
void g_rand_free(GRand *rand_);
gint32 g_rand_int_range(GRand *rand_, gint32 begin, gint32 end);

/* ---- error, spawn: the fork_exec path only ----------------------------- */
typedef struct GError {
    guint32 domain;
    gint code;
    gchar *message;
} GError;
void g_error_free(GError *error);

typedef gint GPid;
typedef void (*GSpawnChildSetupFunc)(gpointer user_data);
typedef enum { G_SPAWN_SEARCH_PATH = 1 << 2 } GSpawnFlags;

typedef struct GPollFD {
    gint fd;
    gushort events;
    gushort revents;
} GPollFD;

/* Stubs: DSperate never configures a guestfwd -exec command, so these fail
 * cleanly instead of pulling GLib's process spawning in. ../README.md. */
gboolean g_shell_parse_argv(const gchar *command_line, gint *argcp, gchar ***argvp, GError **error);
gboolean g_spawn_async_with_fds(const gchar *working_directory, gchar **argv,
                                gchar **envp, GSpawnFlags flags,
                                GSpawnChildSetupFunc child_setup, gpointer user_data,
                                GPid *child_pid, gint stdin_fd, gint stdout_fd,
                                gint stderr_fd, GError **error);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSPERATE_GLIB_SHIM_H */
