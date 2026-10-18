/* Test stand-in for Apple's <OpenGL/OpenGL.h>: only what isaacgl.c uses. */
#include <OpenGL/gl3.h>
typedef struct _CGLContextObject *CGLContextObj;
typedef struct _CGLPixelFormatObject *CGLPixelFormatObj;
typedef enum { kCGLNoError = 0, kCGLBadAttribute = 10000 } CGLError;
typedef enum { kCGLPFAAuxBuffers = 7, kCGLPFAColorSize = 8, kCGLPFAAlphaSize = 11, kCGLPFADepthSize = 12,
    kCGLPFAStencilSize = 13, kCGLPFAAccumSize = 14, kCGLPFAMinimumPolicy = 51, kCGLPFASampleBuffers = 55,
    kCGLPFASamples = 56, kCGLPFARendererID = 70, kCGLPFASingleRenderer = 71, kCGLPFADisplayMask = 84,
    kCGLPFAPBuffer = 90, kCGLPFAOpenGLProfile = 99, kCGLPFAVirtualScreenCount = 128 } CGLPixelFormatAttribute;
enum { kCGLOGLPVersion_Legacy = 0x1000, kCGLOGLPVersion_3_2_Core = 0x3200, kCGLOGLPVersion_GL4_Core = 0x4100 };
typedef enum { kCGLCESurfaceBackingSize = 305 } CGLContextEnable;
typedef enum { kCGLCPSurfaceBackingSize = 304 } CGLContextParameter;
CGLError CGLChoosePixelFormat(const CGLPixelFormatAttribute *, CGLPixelFormatObj *, GLint *);
CGLError CGLCreateContext(CGLPixelFormatObj, CGLContextObj, CGLContextObj *);
CGLContextObj CGLGetCurrentContext(void);
CGLError CGLSetCurrentContext(CGLContextObj);
CGLContextObj CGLRetainContext(CGLContextObj);
void CGLReleaseContext(CGLContextObj);
CGLError CGLDestroyContext(CGLContextObj);
GLuint CGLGetContextRetainCount(CGLContextObj);
CGLError CGLFlushDrawable(CGLContextObj);
const char *CGLErrorString(CGLError);
CGLError CGLIsEnabled(CGLContextObj, CGLContextEnable, GLint *);
CGLError CGLGetParameter(CGLContextObj, CGLContextParameter, GLint *);
