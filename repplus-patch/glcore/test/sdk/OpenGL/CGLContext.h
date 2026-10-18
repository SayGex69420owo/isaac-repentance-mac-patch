/* Test stand-in for Apple's <OpenGL/CGLContext.h> + gliDispatch.h. Only the dispatch slots
 * isaacgl.c names are declared (by their SDK names); the rest is padding so the table has
 * the real slot count (975). Slots isaacgl.c calls directly carry their real signatures. */
typedef struct __GLIContextRec *GLIContext;
typedef void GLvoid;
typedef struct {
    void (*shader_source_ARB)(void);
    void (*vertex_attrib_pointer_ARB)(void);
    void (*enable_vertex_attrib_array_ARB)(void);
    void (*disable_vertex_attrib_array_ARB)(void);
    void (*draw_arrays)(GLIContext, GLenum, GLint, GLsizei);
    void (*draw_elements)(GLIContext, GLenum, GLsizei, GLenum, const GLvoid *);
    void (*tex_image2D)(void);
    void (*tex_sub_image2D)(void);
    void (*renderbuffer_storage_EXT)(void);
    void (*draw_range_elements)(GLIContext, GLenum, GLuint, GLuint, GLsizei, GLenum, const GLvoid *);
    void (*compile_shader_ARB)(void);
    void (*link_program_ARB)(void);
    void (*get_attrib_location_ARB)(void);
    void (*get_uniform_location_ARB)(void);
    void (*check_framebuffer_status_EXT)(void);
    void (*get_renderbuffer_parameteriv_EXT)(void);
    void (*viewport)(void);
    void (*bind_buffer)(void);
    void (*delete_buffers)(void);
    void (*pad[975 - 19])(void);
} GLIFunctionDispatch;
struct _CGLContextObject { GLIContext rend; GLIFunctionDispatch disp; void *priv; void *stak; };
