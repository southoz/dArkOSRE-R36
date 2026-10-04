/* SPDX-License-Identifier: BSD-3-Clause
 *
 * The implementation behind glib.h: the part of GLib the vendored libslirp
 * calls, in libc terms. ../README.md.
 */
#include "glib.h"

#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* ---- memory ------------------------------------------------------------ */
void *slirp_g_malloc(gsize n_bytes, gboolean zeroed)
{
    void *p;
    if (n_bytes == 0)
        return NULL; /* as GLib: g_malloc(0) is NULL, and g_free(NULL) is fine */
    p = zeroed ? calloc(1, n_bytes) : malloc(n_bytes);
    if (!p)
        slirp_g_abort("out of memory allocating %zu bytes", (size_t)n_bytes);
    return p;
}

void *slirp_g_realloc(void *mem, gsize n_bytes)
{
    void *p;
    if (n_bytes == 0) {
        free(mem);
        return NULL;
    }
    p = realloc(mem, n_bytes);
    if (!p)
        slirp_g_abort("out of memory reallocating %zu bytes", (size_t)n_bytes);
    return p;
}

/* ---- strings ----------------------------------------------------------- */
gchar *g_strdup(const gchar *str)
{
    gsize len;
    gchar *copy;
    if (!str)
        return NULL;
    len = strlen(str) + 1;
    copy = (gchar *)slirp_g_malloc(len, FALSE);
    memcpy(copy, str, len);
    return copy;
}

const gchar *g_strerror(gint errnum)
{
    return strerror(errnum);
}

gboolean g_str_has_prefix(const gchar *str, const gchar *prefix)
{
    if (!str || !prefix)
        return FALSE;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

gint g_ascii_strcasecmp(const gchar *s1, const gchar *s2)
{
    /* ASCII only and locale-independent, which is the point of the
     * g_ascii_ prefix: tolower() would fold differently under a Turkish
     * locale and libslirp compares protocol keywords with this. */
    const guchar *a = (const guchar *)s1, *b = (const guchar *)s2;
    for (;; ++a, ++b) {
        gint ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb || ca == 0)
            return ca - cb;
    }
}

gchar *g_strstr_len(const gchar *haystack, gssize haystack_len, const gchar *needle)
{
    gsize needle_len, i, limit;
    if (!haystack || !needle)
        return NULL;
    if (haystack_len < 0)
        return (gchar *)strstr(haystack, needle);
    needle_len = strlen(needle);
    if (needle_len == 0)
        return (gchar *)haystack;
    if ((gsize)haystack_len < needle_len)
        return NULL;
    limit = (gsize)haystack_len - needle_len;
    for (i = 0; i <= limit; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return (gchar *)(haystack + i);
    }
    return NULL;
}

gsize g_strlcpy(gchar *dest, const gchar *src, gsize dest_size)
{
    gsize src_len = strlen(src);
    if (dest_size > 0) {
        gsize n = src_len < dest_size - 1 ? src_len : dest_size - 1;
        memcpy(dest, src, n);
        dest[n] = '\0';
    }
    return src_len; /* the length it wanted, as strlcpy reports it */
}

guint g_strv_length(gchar **str_array)
{
    guint n = 0;
    if (!str_array)
        return 0;
    while (str_array[n])
        ++n;
    return n;
}

void g_strfreev(gchar **str_array)
{
    gchar **p;
    if (!str_array)
        return;
    for (p = str_array; *p; ++p)
        free(*p);
    free(str_array);
}

const gchar *g_getenv(const gchar *variable)
{
    return getenv(variable);
}

/* ---- GString ----------------------------------------------------------- */
static void g_string_reserve(GString *string, gsize extra)
{
    gsize want = string->len + extra + 1;
    if (want <= string->allocated_len)
        return;
    while (string->allocated_len < want)
        string->allocated_len = string->allocated_len ? string->allocated_len * 2 : 64;
    string->str = (gchar *)slirp_g_realloc(string->str, string->allocated_len);
}

GString *g_string_new(const gchar *init)
{
    GString *string = g_new0(GString, 1);
    gsize len = init ? strlen(init) : 0;
    g_string_reserve(string, len);
    if (len)
        memcpy(string->str, init, len);
    string->len = len;
    string->str[len] = '\0';
    return string;
}

void g_string_append_printf(GString *string, const gchar *format, ...)
{
    va_list ap, ap2;
    int n;
    va_start(ap, format);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    if (n > 0) {
        g_string_reserve(string, (gsize)n);
        vsnprintf(string->str + string->len, (gsize)n + 1, format, ap2);
        string->len += (gsize)n;
    }
    va_end(ap2);
}

gchar *g_string_free(GString *string, gboolean free_segment)
{
    gchar *str = string->str;
    if (free_segment) {
        free(str);
        str = NULL;
    } else if (!str) {
        str = g_strdup(""); /* a never-appended GString still owes a string */
    }
    free(string);
    return str;
}

/* ---- diagnostics ------------------------------------------------------- */
void slirp_g_log(const char *level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "slirp: %s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void slirp_g_abort(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "slirp: ERROR: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}

guint g_parse_debug_string(const gchar *string, const GDebugKey *keys, guint nkeys)
{
    guint result = 0, i;
    const gchar *p;
    if (!string)
        return 0;
    if (g_ascii_strcasecmp(string, "all") == 0) {
        for (i = 0; i < nkeys; ++i)
            result |= keys[i].value;
        return result;
    }
    for (p = string; *p;) {
        const gchar *start = p;
        gsize len;
        while (*p && *p != ':' && *p != ';' && *p != ',' && *p != '\t' && *p != ' ')
            ++p;
        len = (gsize)(p - start);
        for (i = 0; i < nkeys; ++i) {
            if (strlen(keys[i].key) == len && strncmp(keys[i].key, start, len) == 0)
                result |= keys[i].value;
        }
        while (*p == ':' || *p == ';' || *p == ',' || *p == '\t' || *p == ' ')
            ++p;
    }
    return result;
}

/* ---- random ------------------------------------------------------------ */
struct GRand {
    guint32 state;
};

GRand *g_rand_new(void)
{
    GRand *r = g_new0(GRand, 1);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    r->state = (guint32)ts.tv_nsec ^ ((guint32)ts.tv_sec << 8) ^ ((guint32)getpid() << 16);
    if (r->state == 0)
        r->state = 0x9E3779B9u;
    return r;
}

void g_rand_free(GRand *rand_)
{
    free(rand_);
}

gint32 g_rand_int_range(GRand *rand_, gint32 begin, gint32 end)
{
    /* xorshift32: libslirp wants ISNs and ephemeral ports that an observer
     * cannot guess from the last one, not cryptographic randomness. */
    guint32 span, x = rand_->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rand_->state = x;
    if (end <= begin)
        return begin;
    span = (guint32)(end - begin);
    return begin + (gint32)(x % span);
}

/* ---- error, spawn ------------------------------------------------------ */
void g_error_free(GError *error)
{
    if (!error)
        return;
    free(error->message);
    free(error);
}

static void set_unsupported(GError **error, const char *what)
{
    GError *e;
    if (!error)
        return;
    e = g_new0(GError, 1);
    e->message = g_strdup(what);
    *error = e;
}

gboolean g_shell_parse_argv(const gchar *command_line, gint *argcp, gchar ***argvp, GError **error)
{
    (void)command_line;
    if (argcp)
        *argcp = 0;
    if (argvp)
        *argvp = NULL;
    set_unsupported(error, "process spawning is not built into this copy of libslirp");
    return FALSE;
}

gboolean g_spawn_async_with_fds(const gchar *working_directory, gchar **argv,
                                gchar **envp, GSpawnFlags flags,
                                GSpawnChildSetupFunc child_setup, gpointer user_data,
                                GPid *child_pid, gint stdin_fd, gint stdout_fd,
                                gint stderr_fd, GError **error)
{
    (void)working_directory; (void)argv; (void)envp; (void)flags;
    (void)child_setup; (void)user_data; (void)stdin_fd; (void)stdout_fd;
    (void)stderr_fd;
    if (child_pid)
        *child_pid = 0;
    set_unsupported(error, "process spawning is not built into this copy of libslirp");
    return FALSE;
}
