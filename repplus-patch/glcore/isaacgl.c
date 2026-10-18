/*
 * isaacgl.dylib: give Rep+ Apple's native core-profile OpenGL under Wine.
 *
 * Rep+ asks for a default (compatibility) OpenGL context. On macOS, Wine's
 * winemac.drv maps that to Apple's legacy profile (GL 2.1 / GLSL 1.20), and
 * Rep+ shuts down (DECISIONS.md E10). On a forward-compatible core 4.1
 * context Rep+ initialises completely; the only errors come from drawing
 * without a vertex array object bound, which core profiles require (E14).
 *
 * This library is loaded into Wine's host processes with
 * DYLD_INSERT_LIBRARIES and uses dyld interposing on the two CGL calls that
 * winemac.drv makes in create_context() (Wine 11.18):
 *   - CGLChoosePixelFormat: an app-context attribute list without an
 *     OpenGL profile gets kCGLOGLPVersion_GL4_Core (Apple's 4.1 core)
 *     instead of the legacy default; compatibility-only attributes are
 *     dropped (aux buffers, accumulation buffer, pbuffer).
 *   - CGLCreateContext: for such a context, (1) if Wine passed no share
 *     context, share with Wine's own core context, as Wine intends for all
 *     app contexts, (2) bind one vertex array object, so draws work, and
 *     (3) hook the context's glShaderSource dispatch entry: Rep+'s shaders
 *     are GLSL 1.10/1.20 without #version, which Apple's core compiler
 *     rejects (E16), so they get a `#version 410` header with defines
 *     (attribute/varying -> in/out, texture2D -> texture, gl_FragColor ->
 *     an out variable). The shader text itself is not otherwise changed.
 * Wine's own probe/enumeration lists are recognised and left alone: they
 * either carry an explicit profile or kCGLPFASingleRenderer, while
 * create_context() lists start with kCGLPFAMinimumPolicy.
 *
 * It is inert in every process except the one running isaac-ng.exe.
 * Code executed: ARM64-native? No. This is x86_64 host code running under
 * Rosetta inside Wine's process, like winemac.so itself. Neither Wine's files
 * nor the game are modified.
 *
 * Environment:
 *   ISAACGL_FORCE=1   activate regardless of the process name (testing)
 *   ISAACGL_SHARE=0   do not add the share context
 *   ISAACGL_DEBUG=1   log every load and every decision to stderr
 *   ISAACGL_CLIENT_ARRAYS=0  do not emulate client-side vertex/index arrays
 *   ISAACGL_STREAM=subdata|data  upload client arrays with glBufferSubData / glBufferData
 *                     instead of the default unsynchronised map of a ring buffer
 *   ISAACGL_STATS=0   no call statistics (trampolines on every dispatch slot)
 *   ISAACGL_FPS=0     no frame-rate lines in the log (by default one line every 10 s)
 *
 * Build: build-isaacgl.sh (xcrun clang -arch x86_64 -dynamiclib)
 */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLContext.h>
#include <OpenGL/gl3.h>
#include <crt_externs.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_ATTRIBS 128
#define MAX_PENDING 16

static int active, debug, share_enabled = 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* pixel formats we upgraded, until the matching CGLCreateContext */
static CGLPixelFormatObj pending[MAX_PENDING];
/* Wine's first core context created through create_context(): the global context */
static CGLContextObj wine_core_context;

#define LOG(...) fprintf(stderr, "[isaacgl] " __VA_ARGS__)

struct ctx_state;
static struct ctx_state *hook_dispatch(CGLContextObj ctx);
static int client_arrays_enabled, stream_mode, fps_enabled;

/* Time spent per frame in the GL calls isaacgl sees (E35): explains slow frames in the log. */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static struct { double draw, tex, shader; unsigned long tex_n, tex_kpx, shader_n; } fr;
#define PROF_BEGIN double prof_t0_ = fps_enabled ? now_ms() : 0
#define PROF_END(field) do { if (fps_enabled) fr.field += now_ms() - prof_t0_; } while (0)

static int contains_ci(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; s && *s; s++)
        if (!strncasecmp(s, needle, n))
            return 1;
    return 0;
}

__attribute__((constructor)) static void isaacgl_init(void)
{
    char **argv = *_NSGetArgv();
    int argc = *_NSGetArgc();
    const char *e;

    debug = (e = getenv("ISAACGL_DEBUG")) && *e == '1';
    share_enabled = !((e = getenv("ISAACGL_SHARE")) && *e == '0');
    client_arrays_enabled = !((e = getenv("ISAACGL_CLIENT_ARRAYS")) && *e == '0');
    fps_enabled = !((e = getenv("ISAACGL_FPS")) && *e == '0');
    if ((e = getenv("ISAACGL_STREAM")))
        stream_mode = !strcmp(e, "subdata") ? 1 : !strcmp(e, "data") ? 2 : 0;
    for (int i = 0; i < argc; i++)
        if (contains_ci(argv[i], "isaac-ng.exe"))
            active = 1;
    if ((e = getenv("ISAACGL_FORCE")) && *e == '1')
        active = 1;
    if (active || debug)
        LOG("loaded in pid %d (%s %s), active=%d, share=%d, client arrays=%d, stream=%s\n", getpid(),
            argc > 0 ? argv[0] : "?", argc > 1 ? argv[1] : "", active, share_enabled, client_arrays_enabled,
            stream_mode == 1 ? "subdata" : stream_mode == 2 ? "data" : "map");
}

static int takes_value(CGLPixelFormatAttribute a)
{
    switch ((int)a)
    {
    case kCGLPFAAuxBuffers: case kCGLPFAColorSize: case kCGLPFAAlphaSize:
    case kCGLPFADepthSize: case kCGLPFAStencilSize: case kCGLPFAAccumSize:
    case kCGLPFASampleBuffers: case kCGLPFASamples: case kCGLPFARendererID:
    case kCGLPFADisplayMask: case kCGLPFAOpenGLProfile: case kCGLPFAVirtualScreenCount:
        return 1;
    default:
        return 0;
    }
}

/* An attribute list built by winemac's create_context() for a non-core context. */
static int is_legacy_app_request(const CGLPixelFormatAttribute *attribs)
{
    if (!attribs || attribs[0] != kCGLPFAMinimumPolicy)
        return 0;
    for (const CGLPixelFormatAttribute *a = attribs; *a; a += takes_value(*a) ? 2 : 1)
        if (*a == kCGLPFAOpenGLProfile || *a == kCGLPFASingleRenderer)
            return 0;
    return 1;
}

static int is_core_app_request(const CGLPixelFormatAttribute *attribs)
{
    if (!attribs || attribs[0] != kCGLPFAMinimumPolicy)
        return 0;
    for (const CGLPixelFormatAttribute *a = attribs; *a; a += takes_value(*a) ? 2 : 1)
        if (*a == kCGLPFAOpenGLProfile)
            return a[1] != (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy;
    return 0;
}

static CGLPixelFormatObj core_formats[MAX_PENDING]; /* Wine's own core requests, for the share context */
static unsigned next_pending, next_core_format;

/* A ring: when Wine chooses formats it never creates contexts with, the oldest are dropped. */
static void remember(CGLPixelFormatObj *slots, unsigned *next, CGLPixelFormatObj pix)
{
    pthread_mutex_lock(&lock);
    slots[(*next)++ % MAX_PENDING] = pix;
    pthread_mutex_unlock(&lock);
}

static int take(CGLPixelFormatObj *slots, CGLPixelFormatObj pix)
{
    int found = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_PENDING; i++)
        if (pix && slots[i] == pix) { slots[i] = NULL; found = 1; break; }
    pthread_mutex_unlock(&lock);
    return found;
}

static CGLError my_CGLChoosePixelFormat(const CGLPixelFormatAttribute *attribs,
                                        CGLPixelFormatObj *pix, GLint *npix)
{
    CGLPixelFormatAttribute core[MAX_ATTRIBS];
    int n = 0;
    CGLError err;

    if (!active)
        return CGLChoosePixelFormat(attribs, pix, npix);

    if (!is_legacy_app_request(attribs))
    {
        err = CGLChoosePixelFormat(attribs, pix, npix);
        if (err == kCGLNoError && *pix && is_core_app_request(attribs))
            remember(core_formats, &next_core_format, *pix);
        return err;
    }

    for (const CGLPixelFormatAttribute *a = attribs; *a && n < MAX_ATTRIBS - 4; a += takes_value(*a) ? 2 : 1)
    {
        if (*a == kCGLPFAAuxBuffers || *a == kCGLPFAAccumSize || *a == kCGLPFAPBuffer)
            continue; /* not available with a core profile */
        core[n++] = a[0];
        if (takes_value(*a))
            core[n++] = a[1];
    }
    core[n++] = kCGLPFAOpenGLProfile;
    core[n++] = (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core;
    core[n] = 0;

    err = CGLChoosePixelFormat(core, pix, npix);
    if (err == kCGLNoError && *pix)
    {
        remember(pending, &next_pending, *pix);
        if (debug) LOG("legacy context request upgraded to 4.1 core (pixel format %p)\n", (void *)*pix);
        return err;
    }
    LOG("core pixel format failed (%d %s); keeping the legacy request\n", err, CGLErrorString(err));
    return CGLChoosePixelFormat(attribs, pix, npix);
}

static CGLError my_CGLCreateContext(CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj *ctx)
{
    CGLError err;
    int upgraded, wine_core;

    if (!active)
        return CGLCreateContext(pix, share, ctx);

    upgraded = take(pending, pix);
    wine_core = !upgraded && take(core_formats, pix);

    if (upgraded && !share && share_enabled)
    {
        pthread_mutex_lock(&lock);
        share = wine_core_context;
        pthread_mutex_unlock(&lock);
    }

    err = CGLCreateContext(pix, share, ctx);
    if (err != kCGLNoError && upgraded && share && share_enabled)
    {
        LOG("sharing with Wine's core context failed (%d %s); creating unshared\n", err, CGLErrorString(err));
        share = NULL;
        err = CGLCreateContext(pix, NULL, ctx);
    }
    if (err != kCGLNoError || !*ctx)
        return err;

    if (wine_core)
    {
        pthread_mutex_lock(&lock);
        if (!wine_core_context)
        {
            wine_core_context = *ctx;
            CGLRetainContext(wine_core_context);
            if (debug) LOG("Wine core context %p will be the share context\n", (void *)*ctx);
        }
        pthread_mutex_unlock(&lock);
    }

    if (upgraded)
    {
        CGLContextObj old = CGLGetCurrentContext();
        GLuint vao = 0;

        if (CGLSetCurrentContext(*ctx) == kCGLNoError)
        {
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            CGLSetCurrentContext(old);
        }
        hook_dispatch(*ctx);
        LOG("core 4.1 context %p for Rep+ (shared with %p, default VAO %u, GLSL translation on)\n",
            (void *)*ctx, (void *)share, vao);
    }
    return err;
}

/* ---- GLSL 1.10/1.20 -> 4.10 source translation ---------------------------------- */

static const char vs_header[] =
    "#version 410\n"
    "// isaacgl: GLSL 1.x shader compiled as 4.10\n"
    "#define attribute in\n"
    "#define varying out\n";
static const char fs_header[] =
    "#version 410\n"
    "// isaacgl: GLSL 1.x shader compiled as 4.10\n"
    "#define varying in\n";
static const char common_defines[] =
    "#define texture1D texture\n"
    "#define texture2D texture\n"
    "#define texture3D texture\n"
    "#define textureCube texture\n"
    "#define texture2DProj textureProj\n"
    "#define texture2DLod textureLod\n"
    "#define texture2DProjLod textureProjLod\n"
    "#define textureCubeLod textureLod\n"
    "#define shadow2D(s, c) vec4(texture(s, c))\n"
    "#define shadow2DProj(s, c) vec4(textureProj(s, c))\n";
/* explicit location 0, so the output does not depend on the linker's choice */
static const char frag_color_decl[] =
    "layout(location = 0) out vec4 isaacgl_FragColor;\n#define gl_FragColor isaacgl_FragColor\n";
static const char frag_data_decl[] =
    "layout(location = 0) out vec4 isaacgl_FragData[4];\n#define gl_FragData isaacgl_FragData\n";

/* A '#version' directive: only at the start of a line (after optional blanks). */
static char *find_version(char *src)
{
    for (char *p = src; (p = strstr(p, "#version")); p += 8)
    {
        const char *q = p;
        while (q > src && (q[-1] == ' ' || q[-1] == '\t')) q--;
        if (q == src || q[-1] == '\n' || q[-1] == '\r')
            return p;
    }
    return NULL;
}

/* Returns a malloc'ed translated source, or NULL to pass the original through. */
static char *translate_glsl(int is_fragment, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    size_t total = 0, n = 0, size;
    const char *decl = "";
    char *src, *out, *p;

    if (count <= 0 || !strings)
        return NULL;
    for (GLsizei i = 0; i < count; i++)
    {
        if (!strings[i])
            return NULL; /* invalid call: let the driver report it */
        total += (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : strlen(strings[i]);
    }
    if (!(src = malloc(total + 1)))
        return NULL;
    for (GLsizei i = 0; i < count; i++)
    {
        size_t len = (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(src + n, strings[i], len);
        n += len;
    }
    src[n] = 0;

    if (strstr(src, "// isaacgl:"))
    {
        free(src);
        return NULL; /* already translated */
    }
    /* A #version below 140 is not accepted by a core context: comment it out. */
    if ((p = find_version(src)))
    {
        if (atoi(p + 8) >= 140)
        {
            free(src);
            return NULL; /* modern shader: leave it alone */
        }
        p[0] = '/'; p[1] = '/';
    }
    if (is_fragment)
        decl = strstr(src, "gl_FragData") ? frag_data_decl : frag_color_decl;
    size = sizeof(vs_header) + sizeof(fs_header) + sizeof(common_defines) + strlen(decl) + sizeof("#line 1\n") + n;
    if (!(out = malloc(size)))
    {
        free(src);
        return NULL;
    }
    snprintf(out, size, "%s%s%s#line 1\n%s", is_fragment ? fs_header : vs_header, common_defines, decl, src);
    free(src);
    return out;
}

/* ---- per-context state ------------------------------------------------------------------
 * Every upgraded context gets its own record: the driver functions its dispatch slots held
 * before we hooked them (Apple's engine can give each context, and each moment, different
 * ones: E23), the client-array state, and its stream buffer. Hooks find the record from the
 * GLIContext they are called with; contexts we did not upgrade have no record and are
 * passed through untouched. */
typedef void (*shader_source_fn)(GLIContext, uintptr_t, GLsizei, const GLchar *const *, const GLint *);
typedef void (*vap_fn)(GLIContext, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (*va_array_fn)(GLIContext, GLuint);
typedef void (*draw_arrays_fn)(GLIContext, GLenum, GLint, GLsizei);
typedef void (*draw_elements_fn)(GLIContext, GLenum, GLsizei, GLenum, const void *);
typedef void (*draw_range_elements_fn)(GLIContext, GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
typedef void (*tex_image_2d_fn)(GLIContext, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*tex_sub_image_2d_fn)(GLIContext, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*renderbuffer_storage_fn)(GLIContext, GLenum, GLenum, GLsizei, GLsizei);
typedef void (*object_fn)(GLIContext, uintptr_t);
typedef GLint (*location_fn)(GLIContext, uintptr_t, const GLchar *);
typedef GLenum (*cfs_fn)(GLIContext, GLenum);
typedef void (*grp_fn)(GLIContext, GLenum, GLenum, GLint *);
typedef void (*viewport_fn)(GLIContext, GLint, GLint, GLsizei, GLsizei);

struct origs
{
    shader_source_fn shader_source;
    vap_fn vap;
    va_array_fn eva, dva;
    draw_arrays_fn draw_arrays;
    draw_elements_fn draw_elements;
    draw_range_elements_fn draw_range_elements;
    tex_image_2d_fn tex_image_2d;
    tex_sub_image_2d_fn tex_sub_image_2d;
    renderbuffer_storage_fn renderbuffer_storage;
    object_fn compile_shader, link_program;
    location_fn get_attrib_location, get_uniform_location;
    cfs_fn check_framebuffer_status;
    grp_fn get_renderbuffer_parameteriv;
    viewport_fn viewport;
};
/* the first context's functions: used if a record lacks one (a slot that was already ours) */
static struct origs fallback;

#define MAX_ATTRS 16
#define MAX_CTX 8
#define RING_INITIAL ((size_t)4 << 20)
#define RING_MAX ((size_t)256 << 20)

struct client_attr
{
    int client, enabled;
    GLint size;
    GLenum type;
    GLboolean norm;
    GLsizei stride;
    size_t elem;            /* bytes of one element */
    const unsigned char *ptr;
};

struct ctx_state
{
    CGLContextObj ctx;
    GLIContext rend;
    unsigned gen;
    struct origs orig;
    struct client_attr attr[MAX_ATTRS];
    GLuint ring;                        /* stream buffer for client vertices and indices */
    size_t ring_size, ring_pos;
    int ring_mode;                      /* how the current reservation is written */
    unsigned char *scratch, *readback;  /* staging copy; indices read back from a game buffer */
    size_t scratch_size, readback_size;
    int logged;
};
static struct ctx_state states[MAX_CTX];
static unsigned state_gen;
static __thread struct ctx_state *tl_state;   /* last record this thread used */

#define ORIG(st, m) ((st) && (st)->orig.m ? (st)->orig.m : fallback.m)

static struct ctx_state *state_for_rend(GLIContext rend)
{
    struct ctx_state *st = tl_state;
    if (st && rend && st->rend == rend)
        return st;
    for (int i = 0; rend && i < MAX_CTX; i++)
        if (states[i].rend == rend)
            return tl_state = &states[i];
    return NULL;
}

static struct ctx_state *state_for_ctx(CGLContextObj ctx)
{
    struct ctx_state *st = tl_state;
    if (st && ctx && st->ctx == ctx)
        return st;
    for (int i = 0; ctx && i < MAX_CTX; i++)
        if (states[i].ctx == ctx)
            return tl_state = &states[i];
    return NULL;
}

/* Called with 'lock' held. A record left by a destroyed context whose address (or renderer)
 * is reused is reset; when all records are taken, the oldest is reused. */
static void clear_state(struct ctx_state *st)
{
    free(st->scratch);
    free(st->readback);
    memset(st, 0, sizeof(*st));
}

static struct ctx_state *register_context(CGLContextObj ctx)
{
    struct ctx_state *st = NULL;
    for (int i = 0; i < MAX_CTX; i++)
        if (states[i].ctx == ctx || (states[i].rend && states[i].rend == ctx->rend))
        {
            clear_state(&states[i]);   /* left by a destroyed context at the same address */
            if (!st) st = &states[i];
        }
    for (int i = 0; !st && i < MAX_CTX; i++)
        if (!states[i].ctx)
            st = &states[i];
    if (!st)
    {
        st = &states[0];
        for (int i = 1; i < MAX_CTX; i++)
            if (states[i].gen < st->gen) st = &states[i];
        LOG("more than %d upgraded contexts: forgetting context %p\n", MAX_CTX, (void *)st->ctx);
    }
    if (tl_state == st) tl_state = NULL;
    clear_state(st);
    st->ctx = ctx;
    st->rend = ctx->rend;
    st->gen = ++state_gen;
    return st;
}

/* Our hooks call public gl* functions (state queries, buffer uploads), which act on the
 * thread's current context. Apple's GL engine also calls dispatch entries itself, e.g. on
 * the Cocoa main thread while Wine updates a context after a window resize, where no
 * context is current: glGetIntegerv then dereferences NULL (E27, the crash on fullscreen).
 * Hooks only do extra work when called for the current context. */
static int is_current(const struct ctx_state *st)
{
    return st && CGLGetCurrentContext() == st->ctx;
}

/* Name of the glShaderSource slot in GLIFunctionDispatch (gliDispatch.h). Apple's
 * headers use ARB names for the GL 2.0 shader entries; build-isaacgl.sh reads the
 * exact name from the SDK and passes it in. The slot's shader parameter may be a GLuint
 * or a GLhandleARB (a pointer on macOS); both travel in an integer register on x86_64,
 * so it is carried as uintptr_t unchanged. */
#ifndef ISAACGL_SS_FIELD
#define ISAACGL_SS_FIELD shader_source_ARB
#endif
static unsigned translated_shaders;

static void hooked_shader_source(GLIContext rend, uintptr_t shader, GLsizei count,
                                 const GLchar *const *strings, const GLint *lengths)
{
    struct ctx_state *st = state_for_rend(rend);
    shader_source_fn orig = ORIG(st, shader_source);
    GLint type = 0;
    char *src;

    if (!is_current(st)) { orig(rend, shader, count, strings, lengths); return; }
    glGetShaderiv((GLuint)shader, GL_SHADER_TYPE, &type);
    src = translate_glsl(type == GL_FRAGMENT_SHADER, count, strings, lengths);
    if (!src)
    {
        orig(rend, shader, count, strings, lengths);
        return;
    }
    if (translated_shaders++ < 3 || debug)
        LOG("shader %u (%s) translated to GLSL 4.10\n", (unsigned)shader, type == GL_FRAGMENT_SHADER ? "fragment" : "vertex");
    {
        const GLchar *one = src;
        orig(rend, shader, 1, &one, NULL);
    }
    free(src);
}

/* ---- client-side vertex/index arrays (E18; streaming rewritten in E31) -----------------
 * Compatibility profiles let glVertexAttribPointer/glDrawElements read vertex and index data
 * straight from application memory (no buffer bound). Core profiles reject such draws with
 * GL_INVALID_OPERATION, which leaves the screen black. The hooks below remember client
 * pointers and, at draw time, copy just the vertices the draw uses, plus its indices, into
 * one stream buffer per context with a single upload:
 *   - attributes whose memory overlaps (Rep+'s interleaved vertices) are copied as one block;
 *   - only vertices [min index, max index] are copied, and the indices are rebased to start
 *     at 0 (unless an enabled attribute comes from one of the game's own buffers);
 *   - the stream buffer is a ring: each draw writes behind the previous one through an
 *     unsynchronised map, and when the ring is full its storage is orphaned (glBufferData
 *     with NULL), so the GPU never waits and never sees data being overwritten.
 * ISAACGL_STREAM=subdata (glBufferSubData) or =data (glBufferData per draw) select simpler
 * upload paths for comparison. GL_QUADS are drawn as triangles through the same buffer. */
#ifndef ISAACGL_VAP_FIELD
#define ISAACGL_VAP_FIELD vertex_attrib_pointer_ARB
#endif
#ifndef ISAACGL_EVA_FIELD
#define ISAACGL_EVA_FIELD enable_vertex_attrib_array_ARB
#endif
#ifndef ISAACGL_DVA_FIELD
#define ISAACGL_DVA_FIELD disable_vertex_attrib_array_ARB
#endif

enum { STREAM_MAP, STREAM_SUBDATA, STREAM_DATA };
static const char *const stream_names[] = { "map", "subdata", "data" };

static void check_hooks(struct ctx_state *st);  /* re-install hooks the driver replaced (E23) */
static unsigned long n_rehooks, n_export_draws;

static unsigned long n_draws, n_emulated, n_errors, n_checked, n_stream_bytes, n_orphans;

static size_t type_size(GLenum type)
{
    switch (type)
    {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4; /* GL_INT, GL_UNSIGNED_INT, GL_FLOAT, GL_FIXED, packed 2_10_10_10 */
    }
}

static void hooked_vap(GLIContext rend, GLuint index, GLint size, GLenum type, GLboolean norm,
                       GLsizei stride, const void *ptr)
{
    struct ctx_state *st = state_for_rend(rend);
    GLint buffer = 0;

    if (!is_current(st)) { ORIG(st, vap)(rend, index, size, type, norm, stride, ptr); return; }
    check_hooks(st);
    /* only valid calls are remembered; anything else goes to the driver, which reports it */
    if (client_arrays_enabled && index < MAX_ATTRS && ptr && stride >= 0 &&
        ((size >= 1 && size <= 4) || size == GL_BGRA))
    {
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
        if (!buffer)
        {
            struct client_attr *a = &st->attr[index];
            a->client = 1; a->size = size; a->type = type; a->norm = norm; a->stride = stride; a->ptr = ptr;
            a->elem = (size == GL_BGRA || type == GL_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_2_10_10_10_REV)
                          ? 4 : (size_t)size * type_size(type);
            return; /* pointed at the stream buffer at draw time */
        }
    }
    if (index < MAX_ATTRS) st->attr[index].client = 0;
    ORIG(st, vap)(rend, index, size, type, norm, stride, ptr);
}

static void hooked_eva(GLIContext rend, GLuint index)
{
    struct ctx_state *st = state_for_rend(rend);
    if (is_current(st))
    {
        check_hooks(st);
        if (index < MAX_ATTRS) st->attr[index].enabled = 1;
    }
    ORIG(st, eva)(rend, index);
}

static void hooked_dva(GLIContext rend, GLuint index)
{
    struct ctx_state *st = state_for_rend(rend);
    if (is_current(st) && index < MAX_ATTRS) st->attr[index].enabled = 0;
    ORIG(st, dva)(rend, index);
}

static void dump_attachment(GLenum att, const char *name)
{
    GLint type = 0, obj = 0, r = 0, g = 0, b = 0, a = 0, d = 0, st = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, att, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type == GL_NONE)
    {
        LOG("  %s: none\n", name);
        return;
    }
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, att, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &obj);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, att, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &r);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, att, GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &g);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, att, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &b);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, att, GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &a);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, att, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &d);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, att, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &st);
    LOG("  %s: %s %d, bits r%d g%d b%d a%d depth%d stencil%d\n", name,
        type == GL_TEXTURE ? "texture" : type == GL_RENDERBUFFER ? "renderbuffer" : "default", obj, r, g, b, a, d, st);
    if (type == GL_TEXTURE)
    {
        GLint prev = 0, w = 0, h = 0, fmt = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glBindTexture(GL_TEXTURE_2D, (GLuint)obj);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        LOG("  %s: texture %d is %dx%d, internal format 0x%x\n", name, obj, w, h, fmt);
    }
    glGetError(); /* queries on a missing attachment may themselves raise an error */
}

static void dump_framebuffer(void)
{
    GLint fb = 0, rfb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    LOG("framebuffer state: draw fb %d, read fb %d, status 0x%04x (8cd5 complete, 8219 undefined = no window surface,"
        " 8cd6 incomplete attachment, 8cd7 missing attachment, 8cdd unsupported), context %p\n",
        fb, rfb, glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER), (void *)CGLGetCurrentContext());
    if (fb)
    {
        dump_attachment(GL_COLOR_ATTACHMENT0, "color0");
        dump_attachment(GL_DEPTH_ATTACHMENT, "depth");
        dump_attachment(GL_STENCIL_ATTACHMENT, "stencil");
    }
    glGetError();
}

/* Debug builds of the run only (ISAACGL_DEBUG=1): glGetError takes the error away from the
 * game, and costs a call per draw. */
static void check_error(const char *what, GLenum mode)
{
    GLenum err;
    if (!debug) return;
    n_checked++;
    if ((err = glGetError()) != GL_NO_ERROR)
    {
        if (n_errors++ < 12)
        {
            LOG("GL error 0x%04x after %s (mode %u, draw #%lu)\n", err, what, mode, n_draws);
            if (err == GL_INVALID_FRAMEBUFFER_OPERATION)
                dump_framebuffer();
        }
    }
}

static void report(void)
{
    if (n_draws == 10 || n_draws == 50 || n_draws == 500 || n_draws == 5000 || n_draws % 100000 == 0)
        LOG("draws %lu, with client arrays emulated %lu, streamed %lu KB, stream buffer renewed %lu times%s\n",
            n_draws, n_emulated, n_stream_bytes >> 10, n_orphans, debug ? "" : "; GL error checks off (ISAACGL_DEBUG=1)");
}

static int sample(void) { return n_draws <= 300 || n_draws % 5000 == 0; }

#define LEGACY_QUADS 0x0007
#define LEGACY_QUAD_STRIP 0x0008
#define LEGACY_POLYGON 0x0009
static unsigned long n_quads;

/* GL_QUADS / GL_QUAD_STRIP / GL_POLYGON (E24) do not exist in core profiles. Quad strips have
 * the vertex order of triangle strips (an odd last vertex is ignored, as by GL_QUAD_STRIP),
 * polygons of fans; quads are drawn as two triangles each. */
static GLenum core_mode(GLenum mode)
{
    return mode == LEGACY_QUAD_STRIP ? GL_TRIANGLE_STRIP : mode == LEGACY_POLYGON ? GL_TRIANGLE_FAN : mode;
}

static unsigned char *grow(unsigned char **buf, size_t *size, size_t need)
{
    if (need > *size)
    {
        size_t n = *size ? *size : 65536;
        unsigned char *p;
        while (n < need) n *= 2;
        if (!(p = realloc(*buf, n))) return NULL;
        *buf = p;
        *size = n;
    }
    return *buf;
}

/* Reserve 'total' bytes of the context's stream buffer, which is left bound to
 * GL_ARRAY_BUFFER; returns where to write them and their offset in the buffer. */
static unsigned char *ring_begin(struct ctx_state *st, size_t total, size_t *at)
{
    if (!st->ring) glGenBuffers(1, &st->ring);
    glBindBuffer(GL_ARRAY_BUFFER, st->ring);
    st->ring_mode = stream_mode;
    if (st->ring_mode == STREAM_DATA)
    {
        *at = 0;
        return grow(&st->scratch, &st->scratch_size, total);
    }
    if (total > st->ring_size || (total * 4 > st->ring_size && st->ring_size < RING_MAX))
    {
        size_t n = st->ring_size ? st->ring_size : RING_INITIAL;
        while (n < total * 4 && n < RING_MAX) n *= 2;
        st->ring_size = n;
        st->ring_pos = n + 1; /* new storage below */
    }
    if (st->ring_pos + total > st->ring_size)
    {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)st->ring_size, NULL, GL_STREAM_DRAW);
        st->ring_pos = 0;
        n_orphans++;
    }
    *at = st->ring_pos;
    if (st->ring_mode == STREAM_MAP)
    {
        void *p = glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)*at, (GLsizeiptr)total,
                                   GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (p) return p;
        LOG("glMapBufferRange failed (GL error 0x%x); streaming with glBufferSubData from now on\n", glGetError());
        stream_mode = st->ring_mode = STREAM_SUBDATA;
    }
    return grow(&st->scratch, &st->scratch_size, total);
}

static void ring_end(struct ctx_state *st, size_t at, size_t total)
{
    if (st->ring_mode == STREAM_MAP)
        glUnmapBuffer(GL_ARRAY_BUFFER);
    else if (st->ring_mode == STREAM_SUBDATA)
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)at, (GLsizeiptr)total, st->scratch);
    else
    {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)total, st->scratch, GL_STREAM_DRAW);
        st->ring_size = 0; /* the buffer now has exactly this size: re-create the ring if needed */
    }
    st->ring_pos = at + ((total + 15) & ~(size_t)15);
    n_stream_bytes += total;
}

static size_t index_size(GLenum type)
{
    return type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 0;
}

static GLuint index_at(const void *p, size_t isz, size_t i)
{
    return isz == 1 ? ((const GLubyte *)p)[i] : isz == 2 ? ((const GLushort *)p)[i] : ((const GLuint *)p)[i];
}

static void store_index(void *p, size_t osz, size_t i, GLuint v)
{
    if (osz == 1) ((GLubyte *)p)[i] = (GLubyte)v;
    else if (osz == 2) ((GLushort *)p)[i] = (GLushort)v;
    else ((GLuint *)p)[i] = v;
}

static void index_range(const void *p, size_t isz, size_t n, GLuint *lo, GLuint *hi)
{
    GLuint l = 0xffffffffu, h = 0;
#define ISAACGL_SCAN(T) do { const T *s_ = p; for (size_t i = 0; i < n; i++) { GLuint v = s_[i]; if (v < l) l = v; if (v > h) h = v; } } while (0)
    if (isz == 1) ISAACGL_SCAN(GLubyte);
    else if (isz == 2) ISAACGL_SCAN(GLushort);
    else ISAACGL_SCAN(GLuint);
#undef ISAACGL_SCAN
    *lo = l;
    *hi = h;
}

static const unsigned char quad_corner[6] = { 0, 1, 2, 0, 2, 3 };

/* Attribute locations each linked program reads (bit 31 = known). Arrays the current program
 * does not read are not copied: compatibility drivers only fetch what the shader uses, so a game
 * may leave an array enabled whose memory is already gone. */
#define MAX_PROGRAMS 4096
static uint32_t prog_attr_mask[MAX_PROGRAMS];

static void note_program_attribs(GLuint program)
{
    GLint n = 0;
    uint32_t mask = 0x80000000u;
    if (!program || program >= MAX_PROGRAMS) return;
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &n);
    for (GLint i = 0; i < n; i++)
    {
        char name[256];
        GLsizei len = 0;
        GLint size = 0, loc, slots;
        GLenum type = 0;
        glGetActiveAttrib(program, (GLuint)i, sizeof(name), &len, &size, &type, name);
        if (len <= 0 || len >= (GLsizei)sizeof(name) - 1) { mask = 0; break; } /* unknown: copy all */
        if ((loc = glGetAttribLocation(program, name)) < 0) continue;       /* built-in (gl_VertexID ...) */
        slots = (type == GL_FLOAT_MAT2 || type == GL_FLOAT_MAT3 || type == GL_FLOAT_MAT4 ||
                 type == GL_FLOAT_MAT2x3 || type == GL_FLOAT_MAT2x4 || type == GL_FLOAT_MAT3x2 ||
                 type == GL_FLOAT_MAT3x4 || type == GL_FLOAT_MAT4x2 || type == GL_FLOAT_MAT4x3) ? 4 : 1;
        for (GLint k = 0; k < slots * (size > 0 ? size : 1) && loc + k < MAX_ATTRS; k++)
            mask |= 1u << (loc + k);
    }
    prog_attr_mask[program] = mask;
}

static uint32_t active_attribs(void)
{
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (prog > 0 && prog < MAX_PROGRAMS && (prog_attr_mask[prog] & 0x80000000u))
        return prog_attr_mask[prog];
    return 0xffffffffu;
}

/* Draw with emulation if the draw needs it. Returns 1 if the draw was handled here (drawn, or
 * dropped because it cannot be drawn), 0 if it should go to the driver unchanged. */
static int emulate_draw(struct ctx_state *st, GLIContext rend, GLenum mode, GLint first, GLsizei count,
                        GLenum itype, const void *indices, int indexed)
{
    struct client_attr *ca[MAX_ATTRS], *unused[MAX_ATTRS];
    const unsigned char *start[MAX_ATTRS];
    struct { const unsigned char *src; size_t bytes, dst; } seg[MAX_ATTRS];
    int attr_seg[MAX_ATTRS], nca = 0, nunused = 0, nseg = 0, app_attrs = 0, quads = mode == LEGACY_QUADS, client_idx;
    uint32_t used = 0;
    GLint prev_array = 0, prev_element = 0;
    const void *src_idx = indices;
    size_t isz = 0, osz, out_count, off = 0, idx_off = 0, total, at = 0;
    GLuint lo = 0, hi = 0;
    GLenum otype;
    unsigned char *p;

    if (count < 0 || (indexed && !(isz = index_size(itype))) || (!indexed && first < 0))
        return 0; /* an invalid call: the driver reports it */
    if (!count)
        return 1; /* nothing to draw (a core driver may still object to a client index pointer) */
    if (client_arrays_enabled)
        for (int i = 0; i < MAX_ATTRS; i++)
        {
            if (!st->attr[i].enabled) continue;
            if (!st->attr[i].client) { app_attrs = 1; continue; }
            if (!used) used = active_attribs();
            if (used & (1u << i)) ca[nca++] = &st->attr[i];
            else unused[nunused++] = &st->attr[i];
        }
    if (!indexed && !nca && !quads && !nunused)
        return 0;
    if (indexed) glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prev_element);
    client_idx = indexed && !prev_element;
    if (!nca && !quads && !nunused && !(client_idx && client_arrays_enabled))
        return 0;
    if (client_idx && !indices)
        return 0;
    if (indexed && !client_idx)
    {
        /* the indices are in one of the game's buffers: read them back (rare; synchronises) */
        size_t bytes = (size_t)count * isz;
        GLint bsize = 0, mapped = 0;
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &bsize);
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_MAPPED, &mapped);
        if (mapped || bsize < 0 || (uintptr_t)indices > (size_t)bsize || bytes > (size_t)bsize - (uintptr_t)indices)
            return 0; /* out of range or mapped: an invalid draw the driver reports */
        if (!grow(&st->readback, &st->readback_size, bytes)) return 0;
        glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)indices, (GLsizeiptr)bytes, st->readback);
        src_idx = st->readback;
    }

    /* vertices the draw uses */
    if (nca)
    {
        if (indexed) index_range(src_idx, isz, (size_t)count, &lo, &hi);
        else { lo = (GLuint)first; hi = (GLuint)((uint64_t)first + (uint64_t)count - 1); }
        if (app_attrs) lo = 0; /* the game's own buffers are addressed from vertex 0 */
    }
    /* one block per group of overlapping attributes (interleaved vertices are one block) */
    for (int k = 0; k < nca; k++)
    {
        uint64_t stride = ca[k]->stride ? (uint64_t)ca[k]->stride : ca[k]->elem;
        start[k] = ca[k]->ptr + (uint64_t)lo * stride;
    }
    for (int k = 1; k < nca; k++) /* insertion sort by the first byte the draw reads */
        for (int j = k; j > 0 && start[j] < start[j - 1]; j--)
        {
            struct client_attr *t = ca[j]; const unsigned char *u = start[j];
            ca[j] = ca[j - 1]; start[j] = start[j - 1];
            ca[j - 1] = t; start[j - 1] = u;
        }
    for (int k = 0; k < nca; k++)
    {
        struct client_attr *a = ca[k];
        uint64_t stride = a->stride ? (uint64_t)a->stride : a->elem;
        uint64_t bytes = (uint64_t)(hi - lo) * stride + a->elem;
        if (bytes > RING_MAX) goto too_large;
        if (nseg && start[k] <= seg[nseg - 1].src + seg[nseg - 1].bytes)
        {
            /* sorted, so start[k] >= the block's start: only the end can grow */
            uint64_t end = (uint64_t)(start[k] - seg[nseg - 1].src) + bytes;
            if (end > RING_MAX) goto too_large;
            if (end > seg[nseg - 1].bytes) seg[nseg - 1].bytes = (size_t)end;
        }
        else
        {
            seg[nseg].src = start[k];
            seg[nseg].bytes = (size_t)bytes;
            nseg++;
        }
        attr_seg[k] = nseg - 1;
    }
    for (int s = 0; s < nseg; s++) /* keep each block's address alignment (mod 16) */
    {
        off = ((off + 15) & ~(size_t)15) + ((uintptr_t)seg[s].src & 15);
        seg[s].dst = off;
        off += seg[s].bytes;
    }

    /* indices, rebased by 'lo' */
    out_count = quads ? (size_t)(count / 4) * 6 : (size_t)count;
    if (!out_count) return 1; /* fewer than 4 quad corners: nothing to draw */
    if (indexed) otype = itype == GL_UNSIGNED_BYTE ? GL_UNSIGNED_SHORT : itype;
    else otype = (uint64_t)first - lo + (uint64_t)count <= 65536 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    osz = index_size(otype);
    if (indexed || quads)
    {
        idx_off = (off + 15) & ~(size_t)15;
        off = idx_off + out_count * osz;
    }
    total = off ? off : 16; /* only unused arrays: they still need a valid buffer offset */
    if (total > RING_MAX) goto too_large;

    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array);
    if (!(p = ring_begin(st, total, &at)))
    {
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_array);
        return 1;
    }
    for (int s = 0; s < nseg; s++)
        memcpy(p + seg[s].dst, seg[s].src, seg[s].bytes);
    if (indexed && !quads && !lo && osz == isz)
        memcpy(p + idx_off, src_idx, out_count * osz);
    else if (indexed)
        for (size_t i = 0; i < out_count; i++)
            store_index(p + idx_off, osz, i, index_at(src_idx, isz, quads ? 4 * (i / 6) + quad_corner[i % 6] : i) - lo);
    else if (quads)
        for (size_t i = 0; i < out_count; i++)
            store_index(p + idx_off, osz, i, (GLuint)(first - (GLint)lo) + (GLuint)(4 * (i / 6) + quad_corner[i % 6]));
    ring_end(st, at, total);

    for (int k = 0; k < nca; k++)
    {
        struct client_attr *a = ca[k];
        uint64_t stride = a->stride ? (uint64_t)a->stride : a->elem;
        size_t offset = at + seg[attr_seg[k]].dst + (size_t)(a->ptr + (uint64_t)lo * stride - seg[attr_seg[k]].src);
        ORIG(st, vap)(rend, (GLuint)(a - st->attr), a->size, a->type, a->norm, a->stride, (const void *)offset);
    }
    for (int k = 0; k < nunused; k++) /* enabled but not read by the program: any valid buffer offset */
    {
        struct client_attr *a = unused[k];
        ORIG(st, vap)(rend, (GLuint)(a - st->attr), a->size, a->type, a->norm, a->stride, (const void *)at);
    }
    if (!st->logged)
    {
        st->logged = 1;
        LOG("client arrays of context %p streamed through a %zu KB buffer (%s)\n", (void *)st->ctx,
            st->ring_size >> 10, stream_names[st->ring_mode]);
    }
    if (quads && n_quads++ < 3)
        LOG("GL_QUADS draw of %d corners emulated with %zu triangle indices\n", (int)count, out_count);
    if (indexed || quads)
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, st->ring);
        ORIG(st, draw_elements)(rend, quads ? GL_TRIANGLES : core_mode(mode), (GLsizei)out_count, otype,
                                (const void *)(at + idx_off));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prev_element);
    }
    else
        ORIG(st, draw_arrays)(rend, core_mode(mode), first - (GLint)lo, count);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_array);
    return 1;

too_large:
    if (n_errors++ < 12)
        LOG("draw #%lu skipped: its client arrays need more than %zu MB (vertices %u..%u)\n",
            n_draws, RING_MAX >> 20, lo, hi);
    return 1;
}

static void hooked_draw_arrays(GLIContext rend, GLenum mode, GLint first, GLsizei count)
{
    struct ctx_state *st = state_for_rend(rend);
    if (mode == LEGACY_QUAD_STRIP && count > 0) count &= ~1;
    if (!is_current(st)) { ORIG(st, draw_arrays)(rend, core_mode(mode), first, count); return; }
    n_draws++;
    PROF_BEGIN;
    if (emulate_draw(st, rend, mode, first, count, 0, NULL, 0)) n_emulated++;
    else ORIG(st, draw_arrays)(rend, core_mode(mode), first, count);
    if (sample()) check_error("glDrawArrays", mode);
    PROF_END(draw);
    report();
}

static void hooked_draw_elements(GLIContext rend, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    struct ctx_state *st = state_for_rend(rend);
    if (mode == LEGACY_QUAD_STRIP && count > 0) count &= ~1;
    if (!is_current(st)) { ORIG(st, draw_elements)(rend, core_mode(mode), count, type, indices); return; }
    n_draws++;
    PROF_BEGIN;
    if (emulate_draw(st, rend, mode, 0, count, type, indices, 1)) n_emulated++;
    else ORIG(st, draw_elements)(rend, core_mode(mode), count, type, indices);
    if (sample()) check_error("glDrawElements", mode);
    PROF_END(draw);
    report();
}

static void hooked_draw_range_elements(GLIContext rend, GLenum mode, GLuint start, GLuint end, GLsizei count,
                                       GLenum type, const void *indices)
{
    struct ctx_state *st = state_for_rend(rend);
    if (mode == LEGACY_QUAD_STRIP && count > 0) count &= ~1;
    if (!is_current(st)) { ORIG(st, draw_range_elements)(rend, core_mode(mode), start, end, count, type, indices); return; }
    n_draws++;
    PROF_BEGIN;
    if (emulate_draw(st, rend, mode, 0, count, type, indices, 1)) n_emulated++; /* range implied */
    else ORIG(st, draw_range_elements)(rend, core_mode(mode), start, end, count, type, indices);
    if (sample()) check_error("glDrawRangeElements", mode);
    PROF_END(draw);
    report();
}

/* ---- legacy texture formats (E19) -----------------------------------------------------
 * GL_ALPHA / GL_LUMINANCE / GL_LUMINANCE_ALPHA / GL_INTENSITY and the old "number of
 * components" internal formats do not exist in core profiles: glTexImage2D fails, the
 * texture stays empty, and a framebuffer using it is incomplete. Map them to R8/RG8 with a
 * texture swizzle that reproduces the legacy channel layout. */
static unsigned n_tex_logged, n_rb_logged;
static int any_swizzle;

#define LEGACY_ALPHA 0x1906
#define LEGACY_LUMINANCE 0x1909
#define LEGACY_LUMINANCE_ALPHA 0x190A
#define LEGACY_INTENSITY 0x8049
#define LEGACY_ALPHA8 0x803C
#define LEGACY_LUMINANCE8 0x8040
#define LEGACY_LUMINANCE8_ALPHA8 0x8045
#define LEGACY_INTENSITY8 0x804B

/* Returns 1 and fills the core format + swizzle if 'fmt' is a legacy (internal) format. */
static int map_legacy_format(GLint fmt, GLint *core_internal, GLenum *core_format, GLint swz[4])
{
    switch (fmt)
    {
    case 1: case LEGACY_LUMINANCE: case LEGACY_LUMINANCE8:
        *core_internal = GL_R8; *core_format = GL_RED;
        swz[0] = swz[1] = swz[2] = GL_RED; swz[3] = GL_ONE; return 1;
    case 2: case LEGACY_LUMINANCE_ALPHA: case LEGACY_LUMINANCE8_ALPHA8:
        *core_internal = GL_RG8; *core_format = GL_RG;
        swz[0] = swz[1] = swz[2] = GL_RED; swz[3] = GL_GREEN; return 1;
    case LEGACY_ALPHA: case LEGACY_ALPHA8:
        *core_internal = GL_R8; *core_format = GL_RED;
        swz[0] = swz[1] = swz[2] = GL_ZERO; swz[3] = GL_RED; return 1;
    case LEGACY_INTENSITY: case LEGACY_INTENSITY8:
        *core_internal = GL_R8; *core_format = GL_RED;
        swz[0] = swz[1] = swz[2] = swz[3] = GL_RED; return 1;
    default:
        return 0;
    }
}

/* Swizzle that turns RGBA data stored as RGBA8 into the legacy internal format's channels. */
static int legacy_rgba_swizzle(GLint fmt, GLint swz[4])
{
    switch (fmt)
    {
    case 1: case LEGACY_LUMINANCE: case LEGACY_LUMINANCE8:
        swz[0] = swz[1] = swz[2] = GL_RED; swz[3] = GL_ONE; return 1;
    case 2: case LEGACY_LUMINANCE_ALPHA: case LEGACY_LUMINANCE8_ALPHA8:
        swz[0] = swz[1] = swz[2] = GL_RED; swz[3] = GL_ALPHA; return 1;
    case LEGACY_ALPHA: case LEGACY_ALPHA8:
        swz[0] = swz[1] = swz[2] = GL_ZERO; swz[3] = GL_ALPHA; return 1;
    case LEGACY_INTENSITY: case LEGACY_INTENSITY8:
        swz[0] = swz[1] = swz[2] = swz[3] = GL_RED; return 1;
    default:
        return 0;
    }
}

static GLenum binding_target(GLenum target)
{
    return (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) ? GL_TEXTURE_CUBE_MAP : target;
}

static int is_proxy(GLenum target)
{
    return target == GL_PROXY_TEXTURE_2D || target == GL_PROXY_TEXTURE_CUBE_MAP ||
           target == GL_PROXY_TEXTURE_RECTANGLE || target == GL_PROXY_TEXTURE_1D_ARRAY;
}

static void hooked_tex_image_2d(GLIContext rend, GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                                GLint border, GLenum format, GLenum type, const void *pixels)
{
    static const GLint identity[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
    struct ctx_state *st = state_for_rend(rend);
    GLint core_internal = internal, swz[4], tmp_internal, tmp_swz[4];
    GLenum core_format = format, tmp_format;
    int legacy = 0;

    if (!is_current(st)) { ORIG(st, tex_image_2d)(rend, target, level, internal, w, h, border, format, type, pixels); return; }
    PROF_BEGIN;
    fr.tex_n++; fr.tex_kpx += ((unsigned long)(w > 0 ? w : 0) * (unsigned long)(h > 0 ? h : 0)) >> 10;
    if (map_legacy_format(internal, &tmp_internal, &tmp_format, tmp_swz))
    {
        legacy = 1;
        if (map_legacy_format((GLint)format, &core_internal, &core_format, swz))
        {
            /* legacy data for a legacy texture (the usual case): R8/RG8 + the internal format's swizzle */
            map_legacy_format(internal, &core_internal, &tmp_format, swz);
        }
        else
        {
            /* RGB(A) data for a legacy texture: keep all channels, pick the legacy ones by swizzle */
            core_internal = GL_RGBA8;
            legacy_rgba_swizzle(internal, swz);
        }
    }
    else
    {
        if (internal == 3) core_internal = GL_RGB8;
        else if (internal == 4 || internal == GL_BGRA) core_internal = GL_RGBA8;
        if (map_legacy_format((GLint)format, &tmp_internal, &core_format, swz))
            legacy = 1; /* legacy data for a modern texture: R/RG data + swizzle */
    }

    if (n_tex_logged < 12 && (legacy || core_internal != internal))
    {
        n_tex_logged++;
        LOG("glTexImage2D %dx%d internal 0x%x format 0x%x type 0x%x -> internal 0x%x format 0x%x%s\n", w, h,
            internal, format, type, core_internal, core_format, legacy ? " + swizzle" : "");
    }
    ORIG(st, tex_image_2d)(rend, target, level, core_internal, w, h, border, core_format, type, pixels);
    PROF_END(tex);
    if (is_proxy(target))
        return;
    if (legacy)
    {
        glTexParameteriv(binding_target(target), GL_TEXTURE_SWIZZLE_RGBA, swz);
        any_swizzle = 1;
    }
    else if (any_swizzle && level == 0) /* a texture re-specified with a modern format loses the old swizzle */
        glTexParameteriv(binding_target(target), GL_TEXTURE_SWIZZLE_RGBA, identity);
}

static void hooked_tex_sub_image_2d(GLIContext rend, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                    GLenum format, GLenum type, const void *pixels)
{
    struct ctx_state *st = state_for_rend(rend);
    GLint ci, swz[4];
    GLenum cf = format;
    PROF_BEGIN;
    map_legacy_format((GLint)format, &ci, &cf, swz);
    ORIG(st, tex_sub_image_2d)(rend, target, level, x, y, w, h, cf, type, pixels);
    fr.tex_n++; fr.tex_kpx += ((unsigned long)(w > 0 ? w : 0) * (unsigned long)(h > 0 ? h : 0)) >> 10;
    PROF_END(tex);
}

static void hooked_renderbuffer_storage(GLIContext rend, GLenum target, GLenum internal, GLsizei w, GLsizei h)
{
    struct ctx_state *st = state_for_rend(rend);
    if (n_rb_logged++ < 8)
        LOG("glRenderbufferStorage %dx%d internal 0x%x\n", w, h, internal);
    ORIG(st, renderbuffer_storage)(rend, target, internal, w, h);
}

/* ---- shader compile/link and location diagnostics (E21) ----------------------------------
 * Run 19 showed Rep+ preparing every draw (attribute pointers, uniforms, program binds) but
 * never issuing one on Apple GL. A program that fails to compile or link, or an attribute it
 * cannot find, would make KAGE skip its draws. Log every failure with the driver's info log
 * and the source lines it names, and the names whose location lookup returns -1. */
static unsigned long n_compiled, n_compile_failed, n_linked, n_link_failed, n_attrib_missing, n_uniform_missing;
static unsigned n_logs_printed;

static void log_text(const char *what, const char *text, int max_lines)
{
    const char *p = text;
    for (int n = 0; p && *p && n < max_lines; n++)
    {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len) LOG("  %s| %.*s\n", what, len > 220 ? 220 : len, p);
        p = e ? e + 1 : NULL;
    }
}

/* Print the original-source lines that an info log names ("ERROR: 0:<line>: ..."). */
static void log_error_lines(GLuint shader, const char *info)
{
    GLint len = 0;
    char *src, *body;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &len);
    if (len <= 0 || !(src = malloc((size_t)len + 1))) return;
    glGetShaderSource(shader, len, NULL, src);
    src[len] = 0;
    body = strstr(src, "#line 1\n");
    body = body ? body + 8 : src;
    for (const char *p = info; (p = strstr(p, ": 0:")); p += 4)
    {
        int want = atoi(p + 4), line = 1;
        const char *s = body;
        while (*s && line < want)
            if (*s++ == '\n') line++;
        if (*s && want > 0)
        {
            const char *e = strchr(s, '\n');
            int l = e ? (int)(e - s) : (int)strlen(s);
            LOG("  source line %d| %.*s\n", want, l > 200 ? 200 : l, s);
        }
    }
    free(src);
}

static void hooked_compile_shader(GLIContext rend, uintptr_t shader)
{
    struct ctx_state *st = state_for_rend(rend);
    if (!is_current(st)) { ORIG(st, compile_shader)(rend, shader); return; }
    GLint ok = 0, type = 0, len = 0;
    PROF_BEGIN;
    ORIG(st, compile_shader)(rend, shader);
    PROF_END(shader); fr.shader_n++;
    glGetShaderiv((GLuint)shader, GL_COMPILE_STATUS, &ok);
    if (ok) { n_compiled++; return; }
    n_compile_failed++;
    glGetShaderiv((GLuint)shader, GL_SHADER_TYPE, &type);
    glGetShaderiv((GLuint)shader, GL_INFO_LOG_LENGTH, &len);
    LOG("COMPILE FAILED: shader %u (%s), failure #%lu\n", (unsigned)shader,
        type == GL_FRAGMENT_SHADER ? "fragment" : "vertex", n_compile_failed);
    if (n_logs_printed < 8 && len > 0)
    {
        char *info = malloc((size_t)len + 1);
        n_logs_printed++;
        if (info)
        {
            glGetShaderInfoLog((GLuint)shader, len, NULL, info);
            info[len] = 0;
            log_text("info", info, 12);
            log_error_lines((GLuint)shader, info);
            free(info);
        }
    }
}

static void hooked_link_program(GLIContext rend, uintptr_t program)
{
    struct ctx_state *st = state_for_rend(rend);
    if (!is_current(st)) { ORIG(st, link_program)(rend, program); return; }
    GLint ok = 0, len = 0, attribs = 0, uniforms = 0;
    PROF_BEGIN;
    ORIG(st, link_program)(rend, program);
    PROF_END(shader); fr.shader_n++;
    glGetProgramiv((GLuint)program, GL_LINK_STATUS, &ok);
    glGetProgramiv((GLuint)program, GL_ACTIVE_ATTRIBUTES, &attribs);
    glGetProgramiv((GLuint)program, GL_ACTIVE_UNIFORMS, &uniforms);
    if (ok)
    {
        note_program_attribs((GLuint)program);
        if (n_linked++ < 4)
            LOG("program %u linked (%d active attributes, %d active uniforms)\n", (unsigned)program, attribs, uniforms);
        return;
    }
    n_link_failed++;
    glGetProgramiv((GLuint)program, GL_INFO_LOG_LENGTH, &len);
    LOG("LINK FAILED: program %u, failure #%lu\n", (unsigned)program, n_link_failed);
    if (n_logs_printed < 8 && len > 0)
    {
        char *info = malloc((size_t)len + 1);
        n_logs_printed++;
        if (info)
        {
            glGetProgramInfoLog((GLuint)program, len, NULL, info);
            info[len] = 0;
            log_text("info", info, 12);
            free(info);
        }
    }
}

/* remember up to 32 distinct missing names so each is logged once */
static char missing_names[32][48];
static int n_missing_names;

static void note_missing(const char *kind, uintptr_t program, const GLchar *name)
{
    GLint linked = -1;
    char key[48];
    snprintf(key, sizeof(key), "%c%s", kind[0], name ? name : "(null)");
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n_missing_names; i++)
        if (!strcmp(missing_names[i], key)) { pthread_mutex_unlock(&lock); return; }
    if (n_missing_names >= 32) { pthread_mutex_unlock(&lock); return; }
    strcpy(missing_names[n_missing_names++], key);
    pthread_mutex_unlock(&lock);
    glGetProgramiv((GLuint)program, GL_LINK_STATUS, &linked);
    LOG("%s location -1 for '%s' (program %u, link status %d)\n", kind, name ? name : "(null)", (unsigned)program, linked);
    glGetError();
}

static GLint hooked_get_attrib_location(GLIContext rend, uintptr_t program, const GLchar *name)
{
    struct ctx_state *st = state_for_rend(rend);
    if (!is_current(st)) return ORIG(st, get_attrib_location)(rend, program, name);
    GLint loc = ORIG(st, get_attrib_location)(rend, program, name);
    if (loc < 0) { n_attrib_missing++; note_missing("attribute", program, name); }
    return loc;
}

static GLint hooked_get_uniform_location(GLIContext rend, uintptr_t program, const GLchar *name)
{
    struct ctx_state *st = state_for_rend(rend);
    if (!is_current(st)) return ORIG(st, get_uniform_location)(rend, program, name);
    GLint loc = ORIG(st, get_uniform_location)(rend, program, name);
    if (loc < 0) { n_uniform_missing++; note_missing("uniform", program, name); }
    return loc;
}

/* ---- framebuffer status, renderbuffer queries and viewports (E22) ------------------------
 * Run 20: all 33 programs link and every location lookup succeeds, yet no draw is issued.
 * Rep+ queries renderbuffer parameters and framebuffer status every frame; log what the
 * driver answers, since a zero size or an incomplete target would make it skip drawing. */
static unsigned long n_fb_checks, n_fb_incomplete;
static unsigned n_grp_logged, n_vp_logged;
static GLint vp_seen[12][4];

static GLenum hooked_check_framebuffer_status(GLIContext rend, GLenum target)
{
    struct ctx_state *st = state_for_rend(rend);
    if (!is_current(st)) return ORIG(st, check_framebuffer_status)(rend, target);
    static int dumping; /* dump_framebuffer() calls glCheckFramebufferStatus itself */
    GLenum status = ORIG(st, check_framebuffer_status)(rend, target);
    if (dumping) return status;
    n_fb_checks++;
    if (status != GL_FRAMEBUFFER_COMPLETE && n_fb_incomplete++ < 6)
    {
        LOG("glCheckFramebufferStatus(0x%x) = 0x%04x (incomplete #%lu)\n", target, status, n_fb_incomplete);
        dumping = 1;
        dump_framebuffer();
        dumping = 0;
    }
    return status;
}

static void hooked_get_renderbuffer_parameteriv(GLIContext rend, GLenum target, GLenum pname, GLint *params)
{
    struct ctx_state *st = state_for_rend(rend);
    if (!is_current(st)) { ORIG(st, get_renderbuffer_parameteriv)(rend, target, pname, params); return; }
    ORIG(st, get_renderbuffer_parameteriv)(rend, target, pname, params);
    if (params && (n_grp_logged < 12 || (*params == 0 && n_grp_logged < 24)))
    {
        GLint rb = 0;
        n_grp_logged++;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
        LOG("glGetRenderbufferParameteriv(pname 0x%x) = %d (bound renderbuffer %d, context %p)\n",
            pname, *params, rb, (void *)CGLGetCurrentContext());
    }
}

/* E28: fullscreen shows a zoomed-in corner. When Rep+ sets a viewport on the window
 * framebuffer (0), log it together with the GL surface backing size that winemac set on the
 * context (kCGLCPSurfaceBackingSize, wine_updateBackingSize in cocoa_opengl.m), each time either
 * changes: a viewport larger than the backing surface shows exactly such a cut-out. */
static void log_window_surface(GLsizei w, GLsizei h)
{
    static GLint last[5] = { -1, -1, -1, -1, -1 };
    static unsigned n_logged;
    CGLContextObj ctx = CGLGetCurrentContext();
    GLint fb = 0, backing[2] = { 0, 0 }, enabled = 0;

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    if (fb || !ctx) return;
    CGLIsEnabled(ctx, kCGLCESurfaceBackingSize, &enabled);
    if (enabled) CGLGetParameter(ctx, kCGLCPSurfaceBackingSize, backing);
    if (last[0] == w && last[1] == h && last[2] == enabled && last[3] == backing[0] && last[4] == backing[1]) return;
    last[0] = w; last[1] = h; last[2] = enabled; last[3] = backing[0]; last[4] = backing[1];
    if (n_logged++ < 40)
        LOG("window framebuffer: viewport %dx%d, surface backing size %s %dx%d (context %p)\n", w, h,
            enabled ? "set to" : "not set, view size", backing[0], backing[1], (void *)ctx);
}

static void hooked_viewport(GLIContext rend, GLint x, GLint y, GLsizei w, GLsizei h)
{
    struct ctx_state *st = state_for_rend(rend);
    if (!is_current(st)) { ORIG(st, viewport)(rend, x, y, w, h); return; }
    ORIG(st, viewport)(rend, x, y, w, h);
    for (unsigned i = 0; i < n_vp_logged; i++)
        if (vp_seen[i][0] == x && vp_seen[i][1] == y && vp_seen[i][2] == w && vp_seen[i][3] == h)
        {
            log_window_surface(w, h);
            return;
        }
    if (n_vp_logged < 12)
    {
        GLint fb = 0;
        vp_seen[n_vp_logged][0] = x; vp_seen[n_vp_logged][1] = y; vp_seen[n_vp_logged][2] = w; vp_seen[n_vp_logged][3] = h;
        n_vp_logged++;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        LOG("glViewport(%d, %d, %d, %d) with draw framebuffer %d\n", x, y, w, h, fb);
    }
    log_window_surface(w, h);
}

/* ---- generic GL call statistics (E20) -----------------------------------------------------
 * Every non-NULL slot of the context's GLIFunctionDispatch table is routed through a tiny
 * x86_64 trampoline: `lock incq count[i]; jmp *target[i]`. The tail jump leaves all argument
 * registers and the stack untouched, so it works for any signature. A background thread logs
 * the most-called slots every 10 s (names come from the SDK header via build-isaacgl.sh). */
#define MAX_SLOTS 1024
#define TRAMP_SIZE 64
#define TRACE_CAP 65536
__attribute__((visibility("hidden"))) uint64_t isaacgl_counts[MAX_SLOTS];
/* frame trace: while trace_on is set, each call also appends its slot index to trace_buf */
__attribute__((visibility("hidden"))) volatile uint8_t isaacgl_trace_on;
__attribute__((visibility("hidden"))) volatile uint64_t isaacgl_trace_pos;
__attribute__((visibility("hidden"))) uint16_t isaacgl_trace_buf[TRACE_CAP];
__attribute__((visibility("hidden"))) void *isaacgl_targets[MAX_SLOTS];
#ifdef ISAACGL_TEST   /* host-independent unit tests (glcore/test): no Mach-O trampolines */
char isaacgl_tramps[MAX_SLOTS * TRAMP_SIZE];
#else
extern char isaacgl_tramps[] __attribute__((visibility("hidden")));
__asm__(
    ".text\n"
    ".p2align 6\n"
    ".globl _isaacgl_tramps\n"
    ".private_extern _isaacgl_tramps\n"
    "_isaacgl_tramps:\n"
    ".set isaacgl_i, 0\n"
    ".rept 1024\n"
    /* No '$' immediates inside .rept: Apple's assembler treats .rept bodies as macros and
     * '$0'... as macro arguments. r10/r11 are scratch and carry no arguments. */
    "0:lock incq _isaacgl_counts+8*isaacgl_i(%rip)\n"
    "  movzbl _isaacgl_trace_on(%rip), %r11d\n"
    "  testl %r11d, %r11d\n"
    "  jnz 1f\n"
    "  jmp *_isaacgl_targets+8*isaacgl_i(%rip)\n"
    "1:leaq 0b(%rip), %r11\n"
    "  jmp Lisaacgl_record\n"
    "  .p2align 6\n"
    "  .set isaacgl_i, isaacgl_i+1\n"
    ".endr\n"
    "Lisaacgl_record:\n"            /* r11 = stub address -> slot index */
    "  leaq _isaacgl_tramps(%rip), %r10\n"
    "  subq %r10, %r11\n"
    "  shrq $6, %r11\n"
    "  pushq %rax\n"
    "  movl $1, %r10d\n"
    "  lock xaddq %r10, _isaacgl_trace_pos(%rip)\n"
    "  cmpq $65536, %r10\n"
    "  jae 2f\n"
    "  leaq _isaacgl_trace_buf(%rip), %rax\n"
    "  movw %r11w, (%rax,%r10,2)\n"
    "2:popq %rax\n"
    "  leaq _isaacgl_targets(%rip), %r10\n"
    "  jmp *(%r10,%r11,8)\n");
#endif

#ifdef ISAACGL_SLOT_NAMES
#include ISAACGL_SLOT_NAMES   /* static const char *const slot_names[] = { ... }; */
#else
static const char *const slot_names[] = { 0 };
#endif
static int n_slots, stats_started;

static const char *slot_name(int i)
{
    return (i >= 0 && i < (int)(sizeof(slot_names) / sizeof(slot_names[0])) && slot_names[i]) ? slot_names[i] : "?";
}

/* Record about one second of calls, then print the calls of one whole frame (between two
 * swaps), consecutive repeats collapsed, 8 per line. */
static void trace_frame(void)
{
    int swaps[8], n_swaps = 0, from, to, line_n = 0;
    size_t n;
    char line[700];

    isaacgl_trace_pos = 0;
    isaacgl_trace_on = 1;
    for (int t = 0; t < 30 && isaacgl_trace_pos < TRACE_CAP; t++) usleep(100000);
    isaacgl_trace_on = 0;
    usleep(100000);
    n = isaacgl_trace_pos < TRACE_CAP ? (size_t)isaacgl_trace_pos : TRACE_CAP;
    for (size_t i = 0; i < n && n_swaps < 8; i++)
        if (strstr(slot_name(isaacgl_trace_buf[i]), "swap") || !strcmp(slot_name(isaacgl_trace_buf[i]), "flush"))
            swaps[n_swaps++] = (int)i;
    LOG("trace| recorded %zu calls, %d frame ends (swap/flush) among the first ones\n", n, n_swaps);
    if (n_swaps >= 2)
    {
        char lens[200] = "";
        for (int k = 1; k < n_swaps; k++)
            snprintf(lens + strlen(lens), sizeof(lens) - strlen(lens), " %d", swaps[k] - swaps[k - 1]);
        LOG("trace| calls per frame:%s\n", lens);
    }
    from = n_swaps >= 3 ? swaps[1] + 1 : n_swaps >= 1 ? swaps[0] + 1 : 0;
    to = n_swaps >= 3 ? swaps[2] + 1 : (int)n;
    if (to - from > 1500) to = from + 1500;
    line[0] = 0;
    for (int i = from; i < to;)
    {
        int j = i;
        char item[64];
        while (j < to && isaacgl_trace_buf[j] == isaacgl_trace_buf[i]) j++;
        if (j - i > 1) snprintf(item, sizeof(item), "%s x%d", slot_name(isaacgl_trace_buf[i]), j - i);
        else snprintf(item, sizeof(item), "%s", slot_name(isaacgl_trace_buf[i]));
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", line[0] ? ", " : "", item);
        i = j;
        if (++line_n % 8 == 0 || i >= to)
        {
            LOG("trace| %s\n", line);
            line[0] = 0;
        }
    }
    LOG("trace| end of frame\n");
}

static void *stats_thread(void *arg)
{
    static uint64_t last[MAX_SLOTS];
    (void)arg;
    for (int round = 1;; round++)
    {
        int idx[30], n = 0;
        uint64_t delta[MAX_SLOTS];
        sleep(10);
        for (int i = 0; i < n_slots; i++)
        {
            delta[i] = isaacgl_counts[i] - last[i];
            last[i] = isaacgl_counts[i];
        }
        for (int k = 0; k < 30; k++)
        {
            int best = -1;
            for (int i = 0; i < n_slots; i++)
            {
                int used = 0;
                for (int j = 0; j < n; j++) used |= idx[j] == i;
                if (!used && delta[i] && (best < 0 || delta[i] > delta[best])) best = i;
            }
            if (best < 0) break;
            idx[n++] = best;
        }
        LOG("GL calls in the last 10 s (round %d; draws %lu, emulated %lu, errors %lu):\n", round, n_draws, n_emulated, n_errors);
        LOG("  shaders compiled %lu, FAILED %lu; programs linked %lu, FAILED %lu; location -1: attributes %lu, uniforms %lu\n",
            n_compiled, n_compile_failed, n_linked, n_link_failed, n_attrib_missing, n_uniform_missing);
        {
            char draws[600] = "";
            for (int i = 0; i < n_slots; i++)
                if (delta[i] && strstr(slot_name(i), "draw") && !strstr(slot_name(i), "buffer"))
                    snprintf(draws + strlen(draws), sizeof(draws) - strlen(draws), " %s %llu", slot_name(i), (unsigned long long)delta[i]);
            LOG("  draw-family calls:%s; framebuffer checks %lu, incomplete %lu\n", draws[0] ? draws : " none", n_fb_checks, n_fb_incomplete);
            LOG("  exported draw calls (all time) %lu; hooks re-installed after the driver replaced them %lu; quad draws emulated %lu\n", n_export_draws, n_rehooks, n_quads);
        }
        for (int j = 0; j < n; j++)
        {
            int i = idx[j];
            const char *name = slot_name(i);
            LOG("  %8llu  %s [%d]\n", (unsigned long long)delta[i], name, i);
        }
        if (!n) LOG("  (no GL calls)\n");
        if (round == 3) trace_frame();
    }
    return NULL;
}

static void count_all_slots(CGLContextObj ctx)
{
    void **table = (void **)&ctx->disp;
    int total = (int)(sizeof(ctx->disp) / sizeof(void *));
    if (total > MAX_SLOTS) total = MAX_SLOTS;
    n_slots = total;
    for (int i = 0; i < total; i++)
    {
        void *tramp = isaacgl_tramps + TRAMP_SIZE * i;
        if (!table[i] || table[i] == tramp) continue;
        if (!isaacgl_targets[i]) isaacgl_targets[i] = table[i];
        if (table[i] == isaacgl_targets[i]) table[i] = tramp;
    }
    if (!stats_started)
    {
        pthread_t t;
        stats_started = 1;
        if (sizeof(slot_names) / sizeof(slot_names[0]) != (size_t)total)
            LOG("note: %d dispatch slots, %zu names from the SDK header\n", total, sizeof(slot_names) / sizeof(slot_names[0]));
        if (!pthread_create(&t, NULL, stats_thread, NULL)) pthread_detach(t);
    }
}

/* ---- driver-replaced dispatch slots and exported draw entry points (E23) -----------------
 * Run 22 (WINEDEBUG +opengl): Rep+ calls glDrawElements ~350 times a second and Wine forwards
 * every call to OpenGL.framework (unix_thunks.c wow64_gl_glDrawElements), yet after the first
 * draw neither our draw hook nor the slot counter sees one. Presumably Apple's GL engine writes
 * specialised draw functions into ctx->disp at run time, which removes our hook, so the client
 * arrays are never uploaded and every draw fails in the core profile. Two counter-measures:
 *  - check_hooks(), called before every batch (glVertexAttribPointer/glEnableVertexAttribArray),
 *    puts our hooks back and adopts the driver's new function as the one to forward to;
 *  - the exported glDrawElements/glDrawArrays/glDrawRangeElements are interposed and always
 *    dispatch through ctx->disp, i.e. through our hooks. */
static void note_rehook(const char *name, void *now, void *before)
{
    if (n_rehooks++ < 12)
        LOG("driver replaced dispatch slot %s (%p, was forwarding to %p); hook re-installed\n", name, now, before);
}

#define ISAACGL_STR_(x) #x
#define ISAACGL_STR(x) ISAACGL_STR_(x)   /* expands ISAACGL_*_FIELD first */

static int is_tramp(const void *p)
{
    return (const char *)p >= isaacgl_tramps && (const char *)p < isaacgl_tramps + MAX_SLOTS * TRAMP_SIZE;
}

#define REHOOK(field, member, fn) do { \
        void **slot_ = (void **)&ctx->disp.field; \
        int i_ = (int)(slot_ - (void **)&ctx->disp); \
        void *tramp_ = isaacgl_tramps + TRAMP_SIZE * i_; \
        if (st->orig.member && *slot_ && *slot_ != (void *)fn && *slot_ != tramp_) { \
            note_rehook(ISAACGL_STR(field), *slot_, (void *)st->orig.member); \
            st->orig.member = (__typeof__(st->orig.member))*slot_; \
            *slot_ = (i_ < MAX_SLOTS && isaacgl_targets[i_] == (void *)fn) ? tramp_ : (void *)fn; \
        } \
    } while (0)

static void check_hooks(struct ctx_state *st)
{
    CGLContextObj ctx = st->ctx;
    REHOOK(ISAACGL_SS_FIELD, shader_source, hooked_shader_source);
#ifdef ISAACGL_TEX2D_FIELD
    REHOOK(ISAACGL_TEX2D_FIELD, tex_image_2d, hooked_tex_image_2d);
#endif
#ifdef ISAACGL_TEXSUB2D_FIELD
    REHOOK(ISAACGL_TEXSUB2D_FIELD, tex_sub_image_2d, hooked_tex_sub_image_2d);
#endif
#ifdef ISAACGL_LP_FIELD
    REHOOK(ISAACGL_LP_FIELD, link_program, hooked_link_program);
#endif
    REHOOK(ISAACGL_VAP_FIELD, vap, hooked_vap);
    REHOOK(ISAACGL_EVA_FIELD, eva, hooked_eva);
    REHOOK(ISAACGL_DVA_FIELD, dva, hooked_dva);
    REHOOK(draw_arrays, draw_arrays, hooked_draw_arrays);
    REHOOK(draw_elements, draw_elements, hooked_draw_elements);
#ifdef ISAACGL_DRE_FIELD
    REHOOK(ISAACGL_DRE_FIELD, draw_range_elements, hooked_draw_range_elements);
#endif
}

/* Exported draw calls: through our hooks for the contexts we upgraded, unchanged for others. */
static struct ctx_state *export_state(CGLContextObj *ctx)
{
    struct ctx_state *st;
    *ctx = CGLGetCurrentContext();
    if (!active || !*ctx || !(st = state_for_ctx(*ctx)))
        return NULL;
    n_export_draws++;
    check_hooks(st);
    return st;
}

static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    CGLContextObj ctx;
    if (!export_state(&ctx)) { glDrawElements(mode, count, type, indices); return; } /* the real export */
    ctx->disp.draw_elements(ctx->rend, mode, count, type, indices);
}

static void my_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    CGLContextObj ctx;
    if (!export_state(&ctx)) { glDrawArrays(mode, first, count); return; }
    ctx->disp.draw_arrays(ctx->rend, mode, first, count);
}

static void my_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices)
{
#ifdef ISAACGL_DRE_FIELD
    CGLContextObj ctx;
    if (!export_state(&ctx)) { glDrawRangeElements(mode, start, end, count, type, indices); return; }
    ctx->disp.ISAACGL_DRE_FIELD(ctx->rend, mode, start, end, count, type, indices);
#else
    glDrawRangeElements(mode, start, end, count, type, indices);
#endif
}

/* Remember the slot's driver function for this context and put our hook in. Empty slots are
 * left alone; a slot that already holds our hook (or its counting trampoline) keeps the
 * fallback, the first context's function. */
#define HOOK(field, member, fn) do { \
        void *cur_ = (void *)ctx->disp.field; \
        if (cur_ && cur_ != (void *)fn && !is_tramp(cur_)) { \
            st->orig.member = (__typeof__(st->orig.member))cur_; \
            ctx->disp.field = (__typeof__(ctx->disp.field))(void *)fn; \
        } \
        if (!fallback.member) fallback.member = st->orig.member; \
    } while (0)

static struct ctx_state *hook_dispatch(CGLContextObj ctx)
{
    const char *e;
    struct ctx_state *st;
    pthread_mutex_lock(&lock);
    st = register_context(ctx);
    HOOK(ISAACGL_SS_FIELD, shader_source, hooked_shader_source);
    HOOK(ISAACGL_VAP_FIELD, vap, hooked_vap);
    HOOK(ISAACGL_EVA_FIELD, eva, hooked_eva);
    HOOK(ISAACGL_DVA_FIELD, dva, hooked_dva);
    HOOK(draw_arrays, draw_arrays, hooked_draw_arrays);
    HOOK(draw_elements, draw_elements, hooked_draw_elements);
#ifdef ISAACGL_TEX2D_FIELD
    HOOK(ISAACGL_TEX2D_FIELD, tex_image_2d, hooked_tex_image_2d);
#endif
#ifdef ISAACGL_TEXSUB2D_FIELD
    HOOK(ISAACGL_TEXSUB2D_FIELD, tex_sub_image_2d, hooked_tex_sub_image_2d);
#endif
#ifdef ISAACGL_RBSTORAGE_FIELD
    HOOK(ISAACGL_RBSTORAGE_FIELD, renderbuffer_storage, hooked_renderbuffer_storage);
#endif
#ifdef ISAACGL_DRE_FIELD
    HOOK(ISAACGL_DRE_FIELD, draw_range_elements, hooked_draw_range_elements);
#endif
#ifdef ISAACGL_CFS_FIELD
    HOOK(ISAACGL_CFS_FIELD, check_framebuffer_status, hooked_check_framebuffer_status);
#endif
#ifdef ISAACGL_GRP_FIELD
    HOOK(ISAACGL_GRP_FIELD, get_renderbuffer_parameteriv, hooked_get_renderbuffer_parameteriv);
#endif
    HOOK(viewport, viewport, hooked_viewport);
#ifdef ISAACGL_CS_FIELD
    HOOK(ISAACGL_CS_FIELD, compile_shader, hooked_compile_shader);
#endif
#ifdef ISAACGL_LP_FIELD
    HOOK(ISAACGL_LP_FIELD, link_program, hooked_link_program);
#endif
#ifdef ISAACGL_GAL_FIELD
    HOOK(ISAACGL_GAL_FIELD, get_attrib_location, hooked_get_attrib_location);
#endif
#ifdef ISAACGL_GUL_FIELD
    HOOK(ISAACGL_GUL_FIELD, get_uniform_location, hooked_get_uniform_location);
#endif
    if (!((e = getenv("ISAACGL_STATS")) && *e == '0'))
        count_all_slots(ctx);
    pthread_mutex_unlock(&lock);
    return st;
}

/* A destroyed context's record is freed (its stream buffer goes with the context's objects,
 * or stays in the share group until that ends). CGLReleaseContext destroys on the last reference. */
static void forget_context(CGLContextObj ctx)
{
    if (!ctx) return;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CTX; i++)
        if (states[i].ctx == ctx)
        {
            if (debug) LOG("context %p destroyed\n", (void *)ctx);
            clear_state(&states[i]);
        }
    pthread_mutex_unlock(&lock);
    if (tl_state && !tl_state->ctx) tl_state = NULL;
}

static CGLError my_CGLDestroyContext(CGLContextObj ctx)
{
    if (active) forget_context(ctx);
    return CGLDestroyContext(ctx);
}

static void my_CGLReleaseContext(CGLContextObj ctx)
{
    if (active && ctx && CGLGetContextRetainCount(ctx) <= 1) forget_context(ctx);
    CGLReleaseContext(ctx);
}

/* ---- frame rate and slow frames (E35) -----------------------------------------------------
 * Every presented frame passes through CGLFlushDrawable (Wine's flushBuffer). Every 10 s one
 * line: frames per second, the slowest frame, how many frames took longer than 25 ms (visible
 * stutter at 60 Hz), and draws and streamed bytes per frame. Each frame over 25 ms gets its own
 * line splitting its time into draws, texture uploads, shader compiles, the swap and the rest
 * (the game's own work and every other GL call through Wine), so a stutter can be traced to its
 * cause. ISAACGL_FPS=0 turns it off. */
static double fps_last, fps_window_start, fps_slowest;
static unsigned long fps_frames, fps_slow, fps_draws0, fps_bytes0, frame_draws0, frame_orphans0, n_slow_logged;

static void count_frame(double t, double swap_ms)
{
    double dt = fps_last ? t - fps_last : 0;
    fps_last = t;
    if (!fps_window_start) { fps_window_start = t; fps_draws0 = n_draws; fps_bytes0 = n_stream_bytes; goto next; }
    fps_frames++;
    if (dt > fps_slowest) fps_slowest = dt;
    if (dt > 25.0)
    {
        fps_slow++;
        if (n_slow_logged++ < 500)
            LOG("slow frame %.0f ms: draws %.1f ms (%lu), textures %.1f ms (%lu uploads, %lu Kpixels), shaders %.1f ms (%lu), "
                "swap %.1f ms, buffer renewals %lu, rest (game and other GL calls) %.1f ms\n",
                dt, fr.draw, n_draws - frame_draws0, fr.tex, fr.tex_n, fr.tex_kpx, fr.shader, fr.shader_n,
                swap_ms, n_orphans - frame_orphans0, dt - fr.draw - fr.tex - fr.shader - swap_ms);
    }
    if (t - fps_window_start >= 10000.0)
    {
        double secs = (t - fps_window_start) / 1000.0;
        LOG("frames: %.1f fps, slowest %.0f ms, %lu frames over 25 ms; %.0f draws and %.0f KB streamed per frame\n",
            fps_frames / secs, fps_slowest, fps_slow, (double)(n_draws - fps_draws0) / fps_frames,
            (double)(n_stream_bytes - fps_bytes0) / 1024.0 / fps_frames);
        fps_window_start = t; fps_frames = fps_slow = 0; fps_slowest = 0;
        fps_draws0 = n_draws; fps_bytes0 = n_stream_bytes;
    }
next:
    memset(&fr, 0, sizeof(fr));
    frame_draws0 = n_draws;
    frame_orphans0 = n_orphans;
}

static CGLError my_CGLFlushDrawable(CGLContextObj ctx)
{
    double t0;
    CGLError err;
    if (!active || !fps_enabled || !state_for_ctx(ctx)) return CGLFlushDrawable(ctx);
    t0 = now_ms();
    err = CGLFlushDrawable(ctx);
    { double t1 = now_ms(); count_frame(t1, t1 - t0); }
    return err;
}

#ifndef ISAACGL_TEST
typedef struct { const void *replacement; const void *replacee; } interpose_t;
__attribute__((used)) static const interpose_t interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)my_CGLChoosePixelFormat, (const void *)CGLChoosePixelFormat },
    { (const void *)my_CGLCreateContext, (const void *)CGLCreateContext },
    { (const void *)my_CGLDestroyContext, (const void *)CGLDestroyContext },
    { (const void *)my_CGLReleaseContext, (const void *)CGLReleaseContext },
    { (const void *)my_CGLFlushDrawable, (const void *)CGLFlushDrawable },
    { (const void *)my_glDrawElements, (const void *)glDrawElements },
    { (const void *)my_glDrawArrays, (const void *)glDrawArrays },
    { (const void *)my_glDrawRangeElements, (const void *)glDrawRangeElements },
};
#endif
