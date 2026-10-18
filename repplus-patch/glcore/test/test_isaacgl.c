/*
 * Unit tests for isaacgl.c that run anywhere (Linux or macOS), without Wine, Steam, the game
 * or Apple's OpenGL. isaacgl.c is compiled into this file with ISAACGL_TEST, against a small
 * fake core-profile GL that behaves like a strict driver: client-side pointers, client-side
 * indices and GL_QUADS raise errors, exactly as on Apple's 4.1 core profile. Each test feeds
 * the hooks what Rep+ sends and checks what reaches the "driver".
 *
 * Run: repplus-patch/glcore/test/run-tests.sh
 */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../isaacgl.c"

/* ---- fake driver state -------------------------------------------------------------- */
#define NBUF 256
struct fbuf { unsigned char *data; size_t size; int alive, mapped; };
static struct fbuf bufs[NBUF];
static GLuint next_buf = 1, bound_array, bound_element, bound_tex;
struct fattr { int enabled; GLint size; GLenum type; GLboolean norm; GLsizei stride; GLuint buffer; uintptr_t offset; };
static struct fattr fattrs[16];
static GLenum gl_err;
static GLuint cur_program;
static GLenum last_index_type;   /* program 7: attributes "uv" at 0 and "pos" at 1 */
static unsigned long n_buffer_uploads;   /* BufferData with data + BufferSubData + MapBufferRange */
static unsigned long n_upload_bytes;
static void set_err(GLenum e) { if (!gl_err) gl_err = e; }

#define MAXREC 64
struct draw_rec { GLenum mode; int n; float v[MAXREC][3][4]; };
static struct draw_rec last_draw;
static int n_driver_draws;

struct fshader { GLenum type; char *src; };
static struct fshader shaders[64];
static struct { GLint internal; GLenum format; GLint swz[4]; int swz_set; } tex_last;

static struct _CGLContextObject test_ctx, test_ctx2, foreign_ctx;
static CGLContextObj cur_ctx = &test_ctx;
static int argc_val = 1;
static char *argv_val[] = { "isaac-ng.exe", NULL };
static char **argv_ptr = argv_val;
int *_NSGetArgc(void) { return &argc_val; }
char ***_NSGetArgv(void) { return &argv_ptr; }

/* CGL */
CGLContextObj CGLGetCurrentContext(void) { return cur_ctx; }
CGLError CGLSetCurrentContext(CGLContextObj c) { (void)c; return kCGLNoError; }
CGLContextObj CGLRetainContext(CGLContextObj c) { return c; }
static int n_destroyed;
void CGLReleaseContext(CGLContextObj c) { (void)c; n_destroyed++; }
CGLError CGLDestroyContext(CGLContextObj c) { (void)c; n_destroyed++; return kCGLNoError; }
GLuint CGLGetContextRetainCount(CGLContextObj c) { (void)c; return 1; }
static int n_flushes;
CGLError CGLFlushDrawable(CGLContextObj c) { (void)c; n_flushes++; return kCGLNoError; }
const char *CGLErrorString(CGLError e) { (void)e; return "error"; }
CGLError CGLChoosePixelFormat(const CGLPixelFormatAttribute *a, CGLPixelFormatObj *p, GLint *n) { (void)a; *p = NULL; *n = 0; return kCGLBadAttribute; }
CGLError CGLCreateContext(CGLPixelFormatObj p, CGLContextObj s, CGLContextObj *c) { (void)p; (void)s; *c = NULL; return kCGLBadAttribute; }
CGLError CGLIsEnabled(CGLContextObj c, CGLContextEnable e, GLint *v) { (void)c; (void)e; *v = 0; return kCGLNoError; }
CGLError CGLGetParameter(CGLContextObj c, CGLContextParameter p, GLint *v) { (void)c; (void)p; v[0] = v[1] = 0; return kCGLNoError; }

/* ---- public GL functions isaacgl.c calls itself --------------------------------------- */
static GLuint *binding(GLenum target) { return target == GL_ELEMENT_ARRAY_BUFFER ? &bound_element : &bound_array; }
void glGenBuffers(GLsizei n, GLuint *ids) { for (GLsizei i = 0; i < n; i++) { assert(next_buf < NBUF); bufs[next_buf].alive = 1; ids[i] = next_buf++; } }
void glBindBuffer(GLenum target, GLuint b) { assert(!b || bufs[b].alive); *binding(target) = b; }
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    struct fbuf *b = &bufs[*binding(target)];
    (void)usage;
    if (!*binding(target)) { set_err(GL_INVALID_OPERATION); return; }
    free(b->data);
    b->data = calloc(1, (size_t)size + 1);
    b->size = (size_t)size;
    if (data) { memcpy(b->data, data, (size_t)size); n_buffer_uploads++; n_upload_bytes += (size_t)size; }
}
void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
    struct fbuf *b = &bufs[*binding(target)];
    if (!*binding(target) || (size_t)(off + size) > b->size) { set_err(GL_INVALID_VALUE); return; }
    memcpy(b->data + off, data, (size_t)size);
    n_buffer_uploads++;
    n_upload_bytes += (size_t)size;
}
void glGetBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, void *data)
{
    struct fbuf *b = &bufs[*binding(target)];
    if (!*binding(target) || (size_t)(off + size) > b->size) { set_err(GL_INVALID_VALUE); return; }
    memcpy(data, b->data + off, (size_t)size);
}
void *glMapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access)
{
    struct fbuf *b = &bufs[*binding(target)];
    (void)access;
    if (!*binding(target) || b->mapped || (size_t)(off + len) > b->size) { set_err(GL_INVALID_OPERATION); return NULL; }
    b->mapped = 1;
    n_buffer_uploads++;
    n_upload_bytes += (size_t)len;
    memset(b->data + off, 0xEE, (size_t)len);   /* invalidated range: old contents are gone */
    return b->data + off;
}
GLboolean glUnmapBuffer(GLenum target) { struct fbuf *b = &bufs[*binding(target)]; if (!b->mapped) set_err(GL_INVALID_OPERATION); b->mapped = 0; return GL_TRUE; }
void glGetIntegerv(GLenum pname, GLint *v)
{
    switch (pname)
    {
    case GL_ARRAY_BUFFER_BINDING: *v = (GLint)bound_array; break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: *v = (GLint)bound_element; break;
    case GL_TEXTURE_BINDING_2D: *v = (GLint)bound_tex; break;
    case GL_CURRENT_PROGRAM: *v = (GLint)cur_program; break;
    default: *v = 0;
    }
}
GLenum glGetError(void) { GLenum e = gl_err; gl_err = GL_NO_ERROR; return e; }
void glGetShaderiv(GLuint s, GLenum pname, GLint *v)
{
    switch (pname)
    {
    case GL_SHADER_TYPE: *v = (GLint)shaders[s].type; break;
    case GL_COMPILE_STATUS: *v = GL_TRUE; break;
    case GL_SHADER_SOURCE_LENGTH: *v = shaders[s].src ? (GLint)strlen(shaders[s].src) + 1 : 0; break;
    default: *v = 0;
    }
}
void glGetShaderSource(GLuint s, GLsizei n, GLsizei *len, GLchar *out) { snprintf(out, (size_t)n, "%s", shaders[s].src ? shaders[s].src : ""); if (len) *len = (GLsizei)strlen(out); }
void glGetShaderInfoLog(GLuint s, GLsizei n, GLsizei *len, GLchar *out) { (void)s; if (n) out[0] = 0; if (len) *len = 0; }
void glGetProgramiv(GLuint p, GLenum pname, GLint *v)
{
    *v = pname == GL_LINK_STATUS ? GL_TRUE : (pname == GL_ACTIVE_ATTRIBUTES && p == 7) ? 2 : 0;
}
void glGetActiveAttrib(GLuint p, GLuint i, GLsizei n, GLsizei *len, GLint *size, GLenum *type, GLchar *name)
{
    (void)p; *size = 1; *type = i ? GL_FLOAT_VEC3 : GL_FLOAT_VEC2;
    *len = (GLsizei)snprintf(name, (size_t)n, "%s", i ? "pos" : "uv");
}
GLint glGetAttribLocation(GLuint p, const GLchar *name) { return p == 7 ? (!strcmp(name, "pos") ? 1 : !strcmp(name, "uv") ? 0 : -1) : -1; }
void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *v)
{
    struct fbuf *b = &bufs[*binding(target)];
    *v = pname == GL_BUFFER_SIZE ? (GLint)b->size : pname == GL_BUFFER_MAPPED ? b->mapped : 0;
}
void glGetProgramInfoLog(GLuint p, GLsizei n, GLsizei *len, GLchar *out) { (void)p; if (n) out[0] = 0; if (len) *len = 0; }
void glBindTexture(GLenum t, GLuint tex) { (void)t; bound_tex = tex; }
void glTexParameteriv(GLenum t, GLenum pname, const GLint *v) { (void)t; if (pname == GL_TEXTURE_SWIZZLE_RGBA) { memcpy(tex_last.swz, v, sizeof(tex_last.swz)); tex_last.swz_set = 1; } }
void glGetTexLevelParameteriv(GLenum t, GLint l, GLenum p, GLint *v) { (void)t; (void)l; (void)p; *v = 0; }
GLenum glCheckFramebufferStatus(GLenum t) { (void)t; return GL_FRAMEBUFFER_COMPLETE; }
void glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint *v) { (void)t; (void)a; (void)p; *v = 0; }
void glGetRenderbufferParameteriv(GLenum t, GLenum p, GLint *v) { (void)t; (void)p; *v = 0; }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
void glGenVertexArrays(GLsizei n, GLuint *a) { for (GLsizei i = 0; i < n; i++) a[i] = 1; }
void glBindVertexArray(GLuint a) { (void)a; }
/* libGL's exported draw functions dispatch through the current context, as Apple's do */
void glDrawArrays(GLenum m, GLint f, GLsizei c) { cur_ctx->disp.draw_arrays(cur_ctx->rend, m, f, c); }
void glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { cur_ctx->disp.draw_elements(cur_ctx->rend, m, c, t, i); }
void glDrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i) { cur_ctx->disp.draw_range_elements(cur_ctx->rend, m, s, e, c, t, i); }

/* ---- fake driver entry points (what ctx->disp holds before isaacgl hooks it) --------------- */
static void drv_vap(GLIContext r, GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr)
{
    (void)r;
    if (stride < 0) { set_err(GL_INVALID_VALUE); return; }
    if (!bound_array && ptr) { set_err(GL_INVALID_OPERATION); return; }   /* core: no client arrays */
    fattrs[i].size = size; fattrs[i].type = type; fattrs[i].norm = norm; fattrs[i].stride = stride;
    fattrs[i].buffer = bound_array; fattrs[i].offset = (uintptr_t)ptr;
}
static void drv_eva(GLIContext r, GLuint i) { (void)r; fattrs[i].enabled = 1; }
static void drv_dva(GLIContext r, GLuint i) { (void)r; fattrs[i].enabled = 0; }

static void record_vertex(int slot, GLuint vtx)
{
    for (int a = 0; a < 3; a++)
    {
        struct fattr *f = &fattrs[a];
        size_t esz, stride, off;
        if (!f->enabled) continue;
        if (!f->buffer) { set_err(GL_INVALID_OPERATION); return; }
        int comps = f->size == GL_BGRA ? 4 : f->size;
        esz = f->type == GL_FLOAT ? 4 : 1;
        stride = f->stride ? (size_t)f->stride : esz * (size_t)comps;
        off = f->offset + vtx * stride;
        if (off + esz * (size_t)comps > bufs[f->buffer].size) { set_err(GL_INVALID_OPERATION); return; }
        for (int c = 0; c < comps && c < 4; c++)
            last_draw.v[slot][a][c] = f->type == GL_FLOAT ? ((float *)(bufs[f->buffer].data + off))[c]
                                                          : bufs[f->buffer].data[off + c] / (f->norm ? 255.0f : 1.0f);
    }
}
static void drv_draw_arrays(GLIContext r, GLenum mode, GLint first, GLsizei count)
{
    (void)r;
    if (mode > GL_TRIANGLE_FAN) { set_err(GL_INVALID_ENUM); return; }   /* GL_QUADS etc. are gone in core */
    memset(&last_draw, 0, sizeof(last_draw));
    last_draw.mode = mode; last_draw.n = count;
    for (GLsizei i = 0; i < count && i < MAXREC; i++) record_vertex(i, (GLuint)(first + i));
    n_driver_draws++;
}
static void drv_draw_elements(GLIContext r, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    size_t isz = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
    struct fbuf *eb = &bufs[bound_element];
    (void)r;
    if (mode > GL_TRIANGLE_FAN) { set_err(GL_INVALID_ENUM); return; }
    if (!bound_element) { set_err(GL_INVALID_OPERATION); return; }      /* core: no client indices */
    last_index_type = type;
    if ((uintptr_t)indices + (size_t)count * isz > eb->size) { set_err(GL_INVALID_OPERATION); return; }
    memset(&last_draw, 0, sizeof(last_draw));
    last_draw.mode = mode; last_draw.n = count;
    for (GLsizei i = 0; i < count && i < MAXREC; i++)
    {
        const unsigned char *p = eb->data + (uintptr_t)indices + (size_t)i * isz;
        GLuint v = isz == 1 ? *p : isz == 2 ? *(const GLushort *)p : *(const GLuint *)p;
        record_vertex(i, v);
    }
    n_driver_draws++;
}
/* a second driver entry, as Apple's engine installs for another context or swaps in at run time */
static int n_drv_b_draws;
static void drv_draw_elements_b(GLIContext r, GLenum mode, GLsizei count, GLenum type, const void *indices)
{ n_drv_b_draws++; drv_draw_elements(r, mode, count, type, indices); }
static void drv_draw_range_elements(GLIContext r, GLenum mode, GLuint s, GLuint e, GLsizei count, GLenum type, const void *indices)
{ (void)s; (void)e; drv_draw_elements(r, mode, count, type, indices); }
static void drv_shader_source(GLIContext r, uintptr_t sh, GLsizei count, const GLchar *const *str, const GLint *len)
{
    size_t n = 0;
    (void)r;
    for (GLsizei i = 0; i < count; i++) n += (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]);
    free(shaders[sh].src);
    shaders[sh].src = calloc(1, n + 1);
    for (GLsizei i = 0; i < count; i++) strncat(shaders[sh].src, str[i], (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]));
}
static void drv_tex_image_2d(GLIContext r, GLenum t, GLint l, GLint internal, GLsizei w, GLsizei h, GLint b, GLenum format, GLenum type, const void *px)
{ (void)r; (void)t; (void)l; (void)w; (void)h; (void)b; (void)type; (void)px; tex_last.internal = internal; tex_last.format = format; }
static void drv_noop(void) {}

/* ---- helpers calling the hooked table like Wine/libGL would -------------------------------- */
#define CALL(field, type, ...) ((type)(void *)test_ctx.disp.field)(test_ctx.rend, __VA_ARGS__)
static void vap(GLuint i, GLint size, GLenum type, GLsizei stride, const void *p) { CALL(vertex_attrib_pointer_ARB, vap_fn, i, size, type, GL_FALSE, stride, p); }
static void eva(GLuint i) { CALL(enable_vertex_attrib_array_ARB, va_array_fn, i); }
static void dva(GLuint i) { CALL(disable_vertex_attrib_array_ARB, va_array_fn, i); }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void init_ctx(struct _CGLContextObject *c, uintptr_t rend)
{
    void (**slot)(void) = (void (**)(void))&c->disp;
    c->rend = (GLIContext)rend;
    for (size_t i = 0; i < sizeof(c->disp) / sizeof(void *); i++) slot[i] = drv_noop;
    c->disp.vertex_attrib_pointer_ARB = (void (*)(void))(void *)drv_vap;
    c->disp.enable_vertex_attrib_array_ARB = (void (*)(void))(void *)drv_eva;
    c->disp.disable_vertex_attrib_array_ARB = (void (*)(void))(void *)drv_dva;
    c->disp.draw_arrays = drv_draw_arrays;
    c->disp.draw_elements = drv_draw_elements;
    c->disp.draw_range_elements = drv_draw_range_elements;
    c->disp.shader_source_ARB = (void (*)(void))(void *)drv_shader_source;
    c->disp.tex_image2D = (void (*)(void))(void *)drv_tex_image_2d;
    c->disp.bind_buffer = NULL; /* an empty slot must stay empty */
}

static void setup(void)
{
    static int done;
    if (done) return;
    done = 1;
    setenv("ISAACGL_STATS", "0", 1);
    active = 1;
    init_ctx(&test_ctx, 0x1234);
    hook_dispatch(&test_ctx);
}

/* Rep+'s sprite batch: interleaved client vertices (pos xyz + uv, stride 20) and client
 * GL_UNSIGNED_SHORT indices, attribute 1 = position, attribute 0 = texcoord. */
static float quad[4][5];
static void fill_quad(float base) { for (int v = 0; v < 4; v++) for (int c = 0; c < 5; c++) quad[v][c] = base + v * 10 + c; }
static const GLushort quad_idx[6] = { 0, 1, 2, 0, 2, 3 };

static void check_quad_draw(const GLushort *idx, int n, const char *what)
{
    CHECK(last_draw.n == n, "%s: %d indices drawn, expected %d", what, last_draw.n, n);
    for (int i = 0; i < n && i < last_draw.n; i++)
    {
        int v = idx[i], ok = 1;
        for (int c = 0; c < 3; c++) ok &= last_draw.v[i][1][c] == quad[v][c];
        for (int c = 0; c < 2; c++) ok &= last_draw.v[i][0][c] == quad[v][3 + c];
        CHECK(ok, "%s: vertex %d (index %d) has wrong data", what, i, v);
    }
}

static void batch_begin(void)
{
    eva(1); vap(1, 3, GL_FLOAT, 20, &quad[0][0]);
    eva(0); vap(0, 2, GL_FLOAT, 20, &quad[0][3]);
}
static void batch_end(void) { dva(1); dva(0); }

static void test_client_arrays_and_indices(void)
{
    setup();
    fill_quad(1);
    glGetError();
    batch_begin();
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, quad_idx);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error after an emulated draw");
    check_quad_draw(quad_idx, 6, "client arrays");
    CHECK(bound_array == 0 && bound_element == 0, "bindings not restored (array %u, element %u)", bound_array, bound_element);
}

static void test_many_draws_stay_correct(void)
{
    setup();
    for (int k = 0; k < 20000; k++)
    {
        fill_quad((float)k);
        batch_begin();
        my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, quad_idx);
        batch_end();
        if (k % 997 == 0 || k == 19999) check_quad_draw(quad_idx, 6, "draw series");
    }
    CHECK(glGetError() == GL_NO_ERROR, "GL error during a long draw series");
}

static void test_app_buffers_pass_through(void)
{
    GLuint vbo, ebo;
    setup();
    fill_quad(100);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glGenBuffers(1, &ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_idx), quad_idx, GL_STATIC_DRAW);
    eva(1); vap(1, 3, GL_FLOAT, 20, (const void *)0);
    eva(0); vap(0, 2, GL_FLOAT, 20, (const void *)12);
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error for a draw from the game's own buffers");
    check_quad_draw(quad_idx, 6, "app buffers");
    CHECK(bound_array == vbo && bound_element == ebo, "the game's buffer bindings were changed");
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void test_client_arrays_with_app_index_buffer(void)
{
    GLuint ebo;
    setup();
    fill_quad(200);
    glGenBuffers(1, &ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 4 + sizeof(quad_idx), NULL, GL_STATIC_DRAW);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 4, sizeof(quad_idx), quad_idx);
    batch_begin();
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)4);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error: client vertices with the game's index buffer");
    check_quad_draw(quad_idx, 6, "client vertices + app EBO");
    CHECK(bound_element == ebo && bound_array == 0, "bindings changed (array %u, element %u)", bound_array, bound_element);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void test_quads(void)
{
    static const GLushort tri[6] = { 0, 1, 2, 0, 2, 3 };
    setup();
    fill_quad(300);
    batch_begin();
    my_glDrawArrays(LEGACY_QUADS, 0, 4);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error for GL_QUADS");
    CHECK(last_draw.mode == GL_TRIANGLES, "GL_QUADS not drawn as triangles (mode %u)", last_draw.mode);
    check_quad_draw(tri, 6, "GL_QUADS via glDrawArrays");
    batch_begin();
    my_glDrawElements(LEGACY_QUADS, 4, GL_UNSIGNED_SHORT, (const GLushort[]){ 3, 2, 1, 0 });
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error for indexed GL_QUADS");
    check_quad_draw((const GLushort[]){ 3, 2, 1, 3, 1, 0 }, 6, "GL_QUADS via glDrawElements");
    CHECK(bound_array == 0 && bound_element == 0, "bindings not restored after GL_QUADS");
}

static void test_shader_translation(void)
{
    const GLchar *vs = "#version 120\nattribute vec3 pos;\nvarying vec2 uv;\nvoid main() { uv = pos.xy; gl_Position = vec4(pos, 1.0); }\n";
    const GLchar *fs = "varying vec2 uv;\nuniform sampler2D tex;\nvoid main() { gl_FragColor = texture2D(tex, uv); }\n";
    const GLchar *modern = "#version 330 core\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n";
    setup();
    shaders[1].type = GL_VERTEX_SHADER; shaders[2].type = GL_FRAGMENT_SHADER; shaders[3].type = GL_FRAGMENT_SHADER;
    CALL(shader_source_ARB, shader_source_fn, 1, 1, &vs, NULL);
    CALL(shader_source_ARB, shader_source_fn, 2, 1, &fs, NULL);
    CALL(shader_source_ARB, shader_source_fn, 3, 1, &modern, NULL);
    CHECK(strncmp(shaders[1].src, "#version 410", 12) == 0, "vertex shader not upgraded to #version 410");
    CHECK(!strstr(shaders[1].src, "#version 120"), "old #version left active");
    CHECK(strstr(shaders[1].src, "#define attribute in") && strstr(shaders[1].src, "#define varying out"), "vertex keywords not mapped");
    CHECK(strstr(shaders[2].src, "#define varying in") && strstr(shaders[2].src, "out vec4 isaacgl_FragColor"), "fragment output not provided");
    CHECK(strstr(shaders[2].src, "#define texture2D texture"), "texture2D not mapped");
    CHECK(strcmp(shaders[3].src, modern) == 0, "a #version 330 shader was changed");
}

static void test_legacy_texture_formats(void)
{
    static unsigned char px[16];
    setup();
    memset(&tex_last, 0, sizeof(tex_last));
    CALL(tex_image2D, tex_image_2d_fn, GL_TEXTURE_2D, 0, LEGACY_LUMINANCE, 4, 4, 0, LEGACY_LUMINANCE, GL_UNSIGNED_BYTE, px);
    CHECK(tex_last.internal == GL_R8 && tex_last.format == GL_RED, "GL_LUMINANCE not mapped to R8/RED (0x%x/0x%x)", tex_last.internal, tex_last.format);
    CHECK(tex_last.swz_set && tex_last.swz[0] == GL_RED && tex_last.swz[3] == GL_ONE, "luminance swizzle missing");
    memset(&tex_last, 0, sizeof(tex_last));
    CALL(tex_image2D, tex_image_2d_fn, GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(tex_last.internal == GL_RGBA8, "a modern texture format was changed");
    CHECK(tex_last.swz_set && tex_last.swz[0] == GL_RED && tex_last.swz[1] == GL_GREEN && tex_last.swz[2] == GL_BLUE &&
          tex_last.swz[3] == GL_ALPHA, "the legacy swizzle was not reset when the texture got a modern format");
    memset(&tex_last, 0, sizeof(tex_last));
    CALL(tex_image2D, tex_image_2d_fn, GL_PROXY_TEXTURE_2D, 0, LEGACY_ALPHA, 4, 4, 0, LEGACY_ALPHA, GL_UNSIGNED_BYTE, NULL);
    CHECK(tex_last.internal == GL_R8 && !tex_last.swz_set, "a proxy texture query got a swizzle (invalid for proxies)");
}


/* ---- E31: stream buffer, ranges, validation, contexts ------------------------------------ */
static float big[10000][5];
static void fill_big(float base) { for (int v = 0; v < 10000; v++) for (int c = 0; c < 5; c++) big[v][c] = base + v * 10 + c; }
static void big_begin(void) { eva(1); vap(1, 3, GL_FLOAT, 20, &big[0][0]); eva(0); vap(0, 2, GL_FLOAT, 20, &big[0][3]); }

static void check_big_draw(const GLuint *verts, int n, const char *what)
{
    CHECK(last_draw.n == n, "%s: %d vertices drawn, expected %d", what, last_draw.n, n);
    for (int i = 0; i < n && i < last_draw.n && i < MAXREC; i++)
    {
        int v = (int)verts[i], ok = 1;
        for (int c = 0; c < 3; c++) ok &= last_draw.v[i][1][c] == big[v][c];
        for (int c = 0; c < 2; c++) ok &= last_draw.v[i][0][c] == big[v][3 + c];
        CHECK(ok, "%s: vertex %d (index %d) has wrong data", what, i, v);
    }
}

static void test_index_range_is_rebased(void)
{
    static const GLushort idx[6] = { 5005, 5006, 5007, 5005, 5007, 5008 };
    static const GLuint want[6] = { 5005, 5006, 5007, 5005, 5007, 5008 };
    unsigned long bytes0;
    setup();
    fill_big(7);
    glGetError();
    bytes0 = n_upload_bytes;
    big_begin();
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error for a draw using vertices 5005..5008");
    check_big_draw(want, 6, "indices 5005..5008");
    CHECK(n_upload_bytes - bytes0 <= 4 * 20 + 6 * 2 + 32, "%lu bytes uploaded for 4 vertices and 6 indices", n_upload_bytes - bytes0);
}

static void test_draw_arrays_range(void)
{
    static const GLuint want[3] = { 7000, 7001, 7002 };
    unsigned long bytes0;
    setup();
    fill_big(11);
    glGetError();
    bytes0 = n_upload_bytes;
    big_begin();
    my_glDrawArrays(GL_TRIANGLES, 7000, 3);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error for glDrawArrays(first 7000)");
    check_big_draw(want, 3, "glDrawArrays first 7000");
    CHECK(n_upload_bytes - bytes0 <= 3 * 20 + 16, "%lu bytes uploaded for 3 vertices", n_upload_bytes - bytes0);
}

static void test_stream_modes_and_wrap(void)
{
    static const GLuint want[6] = { 0, 9999, 5000, 0, 5000, 1 };
    static const GLushort idx[6] = { 0, 9999, 5000, 0, 5000, 1 };
    setup();
    for (int mode = STREAM_MAP; mode <= STREAM_DATA; mode++)
    {
        unsigned long orphans0 = n_orphans;
        stream_mode = mode;
        for (int k = 0; k < 60; k++) /* 200 KB per draw: the 4 MB ring wraps several times */
        {
            fill_big((float)(k + 1000 * mode));
            big_begin();
            my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
            batch_end();
            if (k % 7 == 0 || k == 59) check_big_draw(want, 6, stream_names[mode]);
        }
        CHECK(glGetError() == GL_NO_ERROR, "GL error with ISAACGL_STREAM=%s", stream_names[mode]);
        if (mode != STREAM_DATA) CHECK(n_orphans > orphans0, "%s: the ring never wrapped", stream_names[mode]);
    }
    stream_mode = STREAM_MAP;
}

static void test_mixed_app_and_client_attrs(void)
{
    static const GLushort idx[3] = { 2, 3, 1 };
    GLuint vbo;
    setup();
    fill_quad(400);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    eva(1); vap(1, 3, GL_FLOAT, 20, (const void *)0);       /* position from the game's buffer */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    eva(0); vap(0, 2, GL_FLOAT, 20, &quad[0][3]);           /* texcoord from client memory */
    my_glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error for mixed buffer/client attributes");
    check_quad_draw(idx, 3, "mixed attributes, indices 1..3 (no rebase)");
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void test_client_indices_with_app_vertices(void)
{
    static const GLushort idx[6] = { 3, 2, 1, 3, 1, 0 };
    GLuint vbo;
    setup();
    fill_quad(500);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    eva(1); vap(1, 3, GL_FLOAT, 20, (const void *)0);
    eva(0); vap(0, 2, GL_FLOAT, 20, (const void *)12);
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error for client indices with buffer vertices");
    check_quad_draw(idx, 6, "client indices, buffer vertices");
    CHECK(bound_array == vbo && bound_element == 0, "bindings changed (array %u, element %u)", bound_array, bound_element);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void test_attribute_validation(void)
{
    static unsigned char colors[4][4] = { { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 10, 11, 12 }, { 13, 14, 15, 16 } };
    unsigned long bytes0;
    setup();
    fill_quad(600);
    glGetError();
    vap(2, 4, GL_FLOAT, -4, &quad[0][0]);
    CHECK(glGetError() == GL_INVALID_VALUE, "a negative stride did not reach the driver's error check");
    batch_begin();
    eva(2); vap(2, GL_BGRA, GL_UNSIGNED_BYTE, 0, colors);
    bytes0 = n_upload_bytes;
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, quad_idx);
    dva(2);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error with a GL_BGRA attribute");
    check_quad_draw(quad_idx, 6, "with a GL_BGRA attribute");
    CHECK(last_draw.v[1][2][0] == 5 && last_draw.v[1][2][3] == 8, "GL_BGRA attribute data wrong (%g..%g)",
          last_draw.v[1][2][0], last_draw.v[1][2][3]);
    CHECK(n_upload_bytes - bytes0 <= 80 + 16 + 16 + 12 + 32, "%lu bytes uploaded", n_upload_bytes - bytes0);
}

static void test_degenerate_draws(void)
{
    int draws0;
    setup();
    fill_quad(700);
    glGetError();
    batch_begin();
    draws0 = n_driver_draws;
    my_glDrawArrays(LEGACY_QUADS, 0, 3);                       /* fewer than 4 corners */
    CHECK(n_driver_draws == draws0 && glGetError() == GL_NO_ERROR, "a 3-corner GL_QUADS draw was not dropped quietly");
    my_glDrawElements(GL_TRIANGLES, 0, GL_UNSIGNED_SHORT, quad_idx);
    CHECK(glGetError() == GL_NO_ERROR, "an empty draw raised an error");
    batch_end();
    fill_big(3);
    big_begin();
    my_glDrawArrays(LEGACY_QUAD_STRIP, 0, 5);
    batch_end();
    CHECK(last_draw.mode == GL_TRIANGLE_STRIP && last_draw.n == 4, "GL_QUAD_STRIP of 5 drawn as mode %u with %d vertices",
          last_draw.mode, last_draw.n);
    CHECK(glGetError() == GL_NO_ERROR, "GL error for GL_QUAD_STRIP");
}

static void test_foreign_context_untouched(void)
{
    unsigned long emulated0;
    setup();
    init_ctx(&foreign_ctx, 0x9999);
    cur_ctx = &foreign_ctx;
    glGetError();
    emulated0 = n_emulated;
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, quad_idx);
    CHECK(n_emulated == emulated0, "a draw on a context isaacgl did not upgrade was emulated");
    CHECK(glGetError() == GL_INVALID_OPERATION, "a foreign context's client-index draw did not reach the driver");
    CHECK(foreign_ctx.disp.draw_elements == drv_draw_elements, "a foreign context's dispatch table was changed");
    cur_ctx = &test_ctx;
}

static void test_two_contexts(void)
{
    int b0;
    setup();
    init_ctx(&test_ctx2, 0x5678);
    test_ctx2.disp.draw_elements = drv_draw_elements_b;
    hook_dispatch(&test_ctx2);
    CHECK(test_ctx2.disp.bind_buffer == NULL, "an empty dispatch slot was filled");
    fill_quad(800);
    cur_ctx = &test_ctx2;
    b0 = n_drv_b_draws;
    glGetError();
    ((va_array_fn)(void *)test_ctx2.disp.enable_vertex_attrib_array_ARB)(test_ctx2.rend, 1);
    ((vap_fn)(void *)test_ctx2.disp.vertex_attrib_pointer_ARB)(test_ctx2.rend, 1, 3, GL_FLOAT, GL_FALSE, 20, &quad[0][0]);
    ((va_array_fn)(void *)test_ctx2.disp.enable_vertex_attrib_array_ARB)(test_ctx2.rend, 0);
    ((vap_fn)(void *)test_ctx2.disp.vertex_attrib_pointer_ARB)(test_ctx2.rend, 0, 2, GL_FLOAT, GL_FALSE, 20, &quad[0][3]);
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, quad_idx);
    CHECK(glGetError() == GL_NO_ERROR, "GL error on the second context");
    CHECK(n_drv_b_draws == b0 + 1, "the second context's draw did not reach its own driver function");
    check_quad_draw(quad_idx, 6, "second context");
    ((va_array_fn)(void *)test_ctx2.disp.disable_vertex_attrib_array_ARB)(test_ctx2.rend, 0);
    ((va_array_fn)(void *)test_ctx2.disp.disable_vertex_attrib_array_ARB)(test_ctx2.rend, 1);
    cur_ctx = &test_ctx;
    batch_begin();
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, quad_idx);
    batch_end();
    CHECK(n_drv_b_draws == b0 + 1, "the first context's draw went to the second context's driver function");
    check_quad_draw(quad_idx, 6, "first context after the second");
}

static void test_driver_swaps_slot(void)
{
    int b0;
    setup();
    fill_quad(900);
    test_ctx.disp.draw_elements = drv_draw_elements_b; /* Apple's engine replaces the entry (E23) */
    b0 = n_drv_b_draws;
    glGetError();
    batch_begin();
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, quad_idx);
    batch_end();
    CHECK(test_ctx.disp.draw_elements == hooked_draw_elements, "hook not re-installed after the driver replaced it");
    CHECK(n_drv_b_draws == b0 + 1, "the draw did not go to the driver's new function");
    CHECK(glGetError() == GL_NO_ERROR, "GL error after a slot swap");
    check_quad_draw(quad_idx, 6, "after a slot swap");
}

static void test_shader_edge_cases(void)
{
    const GLchar *nul[1] = { NULL };
    const GLchar *in_comment = "// needs #version 120 features\nvarying vec2 uv;\nvoid main() { gl_FragColor = vec4(uv, 0.0, 1.0); }\n";
    const GLchar *indented = "  #version 110\nvarying vec2 uv;\nvoid main() { gl_FragData[0] = vec4(uv, 0.0, 1.0); }\n";
    char *out;
    setup();
    CHECK(translate_glsl(0, 1, nul, NULL) == NULL, "a NULL source string was not passed through");
    CHECK(translate_glsl(0, 0, NULL, NULL) == NULL, "an empty source list was not passed through");
    out = translate_glsl(1, 1, &in_comment, NULL);
    CHECK(out && !strncmp(out, "#version 410\n", 13) && strstr(out, "// needs #version 120 features"),
          "a '#version' inside a comment was treated as the directive");
    CHECK(out && strstr(out, "layout(location = 0) out vec4 isaacgl_FragColor;"), "fragment output has no explicit location");
    free(out);
    out = translate_glsl(1, 1, &indented, NULL);
    CHECK(out && strstr(out, "  //ersion 110") && strstr(out, "isaacgl_FragData[4]"), "an indented #version 110 or gl_FragData not handled");
    free(out);
}

/* ---- review findings ------------------------------------------------------------------- */
static void test_index_buffer_out_of_range(void)
{
    GLuint ebo;
    unsigned long emulated0;
    setup();
    fill_quad(1000);
    glGenBuffers(1, &ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 12, quad_idx, GL_STATIC_DRAW);
    glGetError();
    emulated0 = n_emulated;
    batch_begin();
    my_glDrawElements(GL_TRIANGLES, 600, GL_UNSIGNED_SHORT, (const void *)0); /* reads past the buffer */
    batch_end();
    CHECK(n_emulated == emulated0, "a draw reading past the game's index buffer was emulated");
    CHECK(glGetError() != GL_NO_ERROR, "the invalid draw did not reach the driver's error check");
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void test_unused_stale_array_not_read(void)
{
    static const GLushort idx[6] = { 0, 9999, 5000, 0, 5000, 1 };
    static const GLuint want[6] = { 0, 9999, 5000, 0, 5000, 1 };
    float *tiny = malloc(4 * sizeof(float));
    setup();
    CALL(link_program_ARB, object_fn, 7);   /* the hook records which attributes program 7 reads */
    cur_program = 7;
    fill_big(5);
    glGetError();
    eva(2); vap(2, 4, GL_FLOAT, 0, tiny);    /* enabled, but program 7 does not read location 2 */
    free(tiny);                              /* ...and its memory is gone (ASan would catch a read) */
    big_begin();
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
    batch_end();
    dva(2);
    CHECK(glGetError() == GL_NO_ERROR, "GL error with an unused enabled array");
    check_big_draw(want, 6, "unused stale array");
    cur_program = 0;
}

static void test_overlapping_arrays_with_different_strides(void)
{
    static float mem[400];
    setup();
    for (int i = 0; i < 400; i++) mem[i] = (float)i;
    glGetError();
    eva(1); vap(1, 3, GL_FLOAT, 40, mem);                       /* stride 40 */
    eva(0); vap(0, 2, GL_FLOAT, 8, (const char *)mem + 16);     /* stride 8, inside the first array */
    my_glDrawArrays(GL_TRIANGLES, 10, 3);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error for overlapping arrays with different strides");
    CHECK(last_draw.n == 3, "overlapping arrays: %d vertices drawn", last_draw.n);
    for (int i = 0; i < 3 && last_draw.n == 3; i++)
    {
        int v = 10 + i;
        CHECK(last_draw.v[i][1][0] == mem[v * 10] && last_draw.v[i][1][2] == mem[v * 10 + 2], "position of vertex %d wrong", v);
        CHECK(last_draw.v[i][0][0] == mem[4 + 2 * v] && last_draw.v[i][0][1] == mem[5 + 2 * v], "texcoord of vertex %d wrong", v);
    }
}

static void test_byte_indices_widened(void)
{
    static const GLubyte idx[6] = { 0, 1, 2, 0, 2, 3 };
    setup();
    fill_quad(1100);
    glGetError();
    batch_begin();
    my_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, idx);
    batch_end();
    CHECK(glGetError() == GL_NO_ERROR, "GL error for byte indices");
    CHECK(last_index_type == GL_UNSIGNED_SHORT, "byte indices reached the driver as 0x%x", last_index_type);
    check_quad_draw(quad_idx, 6, "byte indices");
}

static void test_mixed_texture_formats(void)
{
    static unsigned char px[64];
    setup();
    memset(&tex_last, 0, sizeof(tex_last));
    CALL(tex_image2D, tex_image_2d_fn, GL_TEXTURE_2D, 0, LEGACY_LUMINANCE_ALPHA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(tex_last.internal == GL_RGBA8 && tex_last.format == GL_RGBA && tex_last.swz[0] == GL_RED && tex_last.swz[3] == GL_ALPHA,
          "LUMINANCE_ALPHA texture from RGBA data: 0x%x/0x%x, alpha from 0x%x", tex_last.internal, tex_last.format, tex_last.swz[3]);
    memset(&tex_last, 0, sizeof(tex_last));
    CALL(tex_image2D, tex_image_2d_fn, GL_TEXTURE_2D, 0, LEGACY_ALPHA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(tex_last.internal == GL_RGBA8 && tex_last.swz[0] == GL_ZERO && tex_last.swz[3] == GL_ALPHA, "ALPHA texture from RGBA data wrong");
    memset(&tex_last, 0, sizeof(tex_last));
    CALL(tex_image2D, tex_image_2d_fn, GL_TEXTURE_2D, 0, 4, 4, 4, 0, LEGACY_LUMINANCE, GL_UNSIGNED_BYTE, px);
    CHECK(tex_last.internal == GL_RGBA8 && tex_last.format == GL_RED && tex_last.swz[0] == GL_RED && tex_last.swz[3] == GL_ONE,
          "internal format 4 with LUMINANCE data: 0x%x/0x%x", tex_last.internal, tex_last.format);
}

static void test_frame_counter(void)
{
    setup();
    for (int i = 0; i < 5; i++) my_CGLFlushDrawable(&test_ctx);
    CHECK(n_flushes == 5 && fps_frames == 4, "frames not passed on or not counted (%d flushed, %lu counted)", n_flushes, fps_frames);
    fps_last = now_ms() - 40;   /* pretend the last frame took 40 ms */
    my_CGLFlushDrawable(&test_ctx);
    CHECK(fps_slow == 1 && n_slow_logged == 1, "a 40 ms frame was not reported as slow");
}

static void test_context_destroyed(void)
{
    setup();
    init_ctx(&test_ctx2, 0x5678);
    hook_dispatch(&test_ctx2);
    CHECK(state_for_ctx(&test_ctx2) != NULL, "second context not registered");
    my_CGLDestroyContext(&test_ctx2);
    CHECK(state_for_ctx(&test_ctx2) == NULL, "a destroyed context kept its record");
    CHECK(state_for_ctx(&test_ctx) != NULL, "destroying one context dropped another");
}

int main(void)
{
    test_client_arrays_and_indices();
    test_many_draws_stay_correct();
    test_app_buffers_pass_through();
    test_client_arrays_with_app_index_buffer();
    test_quads();
    test_shader_translation();
    test_legacy_texture_formats();
    test_index_range_is_rebased();
    test_draw_arrays_range();
    test_stream_modes_and_wrap();
    test_mixed_app_and_client_attrs();
    test_client_indices_with_app_vertices();
    test_attribute_validation();
    test_degenerate_draws();
    test_foreign_context_untouched();
    test_two_contexts();
    test_driver_swaps_slot();
    test_shader_edge_cases();
    test_index_buffer_out_of_range();
    test_unused_stale_array_not_read();
    test_overlapping_arrays_with_different_strides();
    test_byte_indices_widened();
    test_mixed_texture_formats();
    test_frame_counter();
    test_context_destroyed();
    printf("%d checks, %d failures; driver draws %d, buffer uploads %lu (%.2f per draw)\n",
           checks, failures, n_driver_draws, n_buffer_uploads, n_driver_draws ? (double)n_buffer_uploads / n_driver_draws : 0.0);
    return failures != 0;
}
