// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// The half-float picture surface: EGL, GBM and OpenGL, loaded at run time so a
// machine without them still builds and still pins, only in SDR.
//
// The shape of the thing, in one paragraph: one compose texture the size of an
// output holds the picture as it should look, half-float and PQ-encoded; a
// command clears the region that changed, draws the pins into it -- shadow,
// image, rim, in that order -- and copies the whole picture into a dma-buf the
// compositor takes.  The drawing is regional -- a pin that moved by three pixels
// costs three pixels of drawing -- but the copy is not: the compositor may
// re-read a buffer the damage does not cover (it does so when the attached
// buffer changes, which with two buffers alternated is every other commit), and
// a slot that had only ever been handed the damaged regions would then show a
// pin where it used to be.
//
// Three details are worth knowing before reading the shaders:
//
//   * The pixels arrive as the packed ten-bit words the capture wrote, with red
//     in the *high* ten bits (the order DRM names `XRGB2101010`).  GL's
//     `GL_UNSIGNED_INT_2_10_10_10_REV` puts red in the *low* ones, so the image
//     shader swaps the two ends back.  A grey picture looks right either way,
//     which is exactly why it is spelled out here.
//   * A shadow is built in a single-channel texture, so the shader that fills
//     the silhouette writes its coverage into red.  Only the shader that *draws*
//     the finished mask into the picture turns it into a black pixel's alpha.
//   * The compositor reads a surface's alpha as premultiplied, so a pixel this
//     side writes with partial alpha is a pixel the compositor adds to the
//     desktop.  The shadow is black, so multiplying it by anything keeps it
//     black; the rim and the image are opaque except for their antialiased
//     edges, where the colour is written straight rather than multiplied.  That
//     is a half-pixel-wide approximation, and it is what to revisit if a rounded
//     corner ever looks too bright.

#include "pin_hdr_fp16.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<gbm.h>) && __has_include(<EGL/egl.h>) && __has_include(<EGL/eglext.h>) &&         \
    __has_include(<GL/gl.h>) && __has_include(<GL/glext.h>)
#define VSHOT_FP16_HEADERS 1
#endif
#endif

#ifndef VSHOT_FP16_HEADERS
#define VSHOT_FP16_HEADERS 0
#endif

#if VSHOT_FP16_HEADERS
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <gbm.h>
#if __has_include(<drm_fourcc.h>)
#include <drm_fourcc.h>
#endif
#endif

#ifndef DRM_FORMAT_ABGR16161616F
// The fourcc the compositor's feedback carries for half-float RGBA, spelled out
// so a build without libdrm's headers still names the same format.
#define DRM_FORMAT_ABGR16161616F 0x48344241u
#endif

// How many picture buffers one output may hold: two, so one can be with the
// compositor while the next is drawn.  A third would only ever wait.
#define FP16_SLOTS 2

// A box blur of one small pass, twice, is what the SDR shadow is made of; the
// numbers have to agree with `ui/shadow.cpp` for the two to look alike.
#define SHADOW_DOWNSCALE 3
#define SHADOW_PASSES 2

#if !VSHOT_FP16_HEADERS

// Built on a machine without the headers: every entry point reports why, and the
// caller keeps its software path.

struct vshot_fp16 {
    int unused;
};

static void say(char *why, size_t why_len, const char *what) {
    if (why && why_len) {
        snprintf(why, why_len, "%s", what);
    }
}

vshot_fp16 *vshot_fp16_open(const char *render_node, char *why, size_t why_len) {
    (void)render_node;
    say(why, why_len, "built without GBM/EGL/GL headers");
    return NULL;
}
void vshot_fp16_close(vshot_fp16 *ctx) { (void)ctx; }
int vshot_fp16_alloc(vshot_fp16 *ctx, uint32_t w, uint32_t h, uint32_t f, uint64_t m,
                     vshot_fp16_buffer *o, char *why, size_t why_len) {
    (void)ctx;(void)w;(void)h;(void)f;(void)m;(void)o;
    say(why, why_len, "built without GBM/EGL/GL headers");
    return -1;
}
void vshot_fp16_free_buffer(vshot_fp16 *ctx, vshot_fp16_buffer *b) { (void)ctx;(void)b; }
int vshot_fp16_image(vshot_fp16 *ctx, uint64_t id, const uint32_t *w, uint32_t a, uint32_t b) {
    (void)ctx;(void)id;(void)w;(void)a;(void)b; return -1;
}
int vshot_fp16_mask(vshot_fp16 *ctx, uint64_t id, int w, int h, int r, int s, int o, int a) {
    (void)ctx;(void)id;(void)w;(void)h;(void)r;(void)s;(void)o;(void)a; return -1;
}
void vshot_fp16_drop(vshot_fp16 *ctx, uint64_t id) { (void)ctx;(void)id; }
int vshot_fp16_begin(vshot_fp16 *ctx, uint32_t w, uint32_t h) { (void)ctx;(void)w;(void)h; return -1; }
void vshot_fp16_clear(vshot_fp16 *ctx, int x, int y, int w, int h) { (void)ctx;(void)x;(void)y;(void)w;(void)h; }
int vshot_fp16_draw_shadow(vshot_fp16 *ctx, uint64_t id, int x, int y, int w, int h) {
    (void)ctx;(void)id;(void)x;(void)y;(void)w;(void)h; return -1;
}
int vshot_fp16_draw_image(vshot_fp16 *ctx, uint64_t id, int x, int y, int w, int h, int r) {
    (void)ctx;(void)id;(void)x;(void)y;(void)w;(void)h;(void)r; return -1;
}
int vshot_fp16_draw_rim(vshot_fp16 *ctx, int x, int y, int w, int h, int r, int t, float a, float b,
                        float c, float alpha) {
    (void)ctx;(void)x;(void)y;(void)w;(void)h;(void)r;(void)t;(void)a;(void)b;(void)c;(void)alpha;
    return -1;
}
int vshot_fp16_present(vshot_fp16 *ctx, int slot, int x, int y, int w, int h) {
    (void)ctx;(void)slot;(void)x;(void)y;(void)w;(void)h; return -1;
}

#else // VSHOT_FP16_HEADERS

// --- the libraries, loaded at run time -------------------------------------

static void *try_open(const char *const *names) {
    for (int i = 0; names[i]; i++) {
        void *handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            return handle;
        }
    }
    return NULL;
}

static void *sym(void *handle, const char *name) { return dlsym(handle, name); }

typedef struct GbmApi {
    struct gbm_device *(*create_device)(int fd);
    void (*device_destroy)(struct gbm_device *dev);
    struct gbm_bo *(*bo_create_with_modifiers)(struct gbm_device *dev, uint32_t width,
                                               uint32_t height, uint32_t format,
                                               const uint64_t *modifiers, const unsigned int count);
    int (*bo_get_fd)(struct gbm_bo *bo);
    uint32_t (*bo_get_stride_for_plane)(struct gbm_bo *bo, int plane);
    uint32_t (*bo_get_offset)(struct gbm_bo *bo, int plane);
    uint64_t (*bo_get_modifier)(struct gbm_bo *bo);
    uint32_t (*bo_get_format)(struct gbm_bo *bo);
    int (*bo_get_plane_count)(struct gbm_bo *bo);
    void (*bo_destroy)(struct gbm_bo *bo);
} GbmApi;

typedef struct EglApi {
    EGLDisplay (*get_platform_display)(EGLenum platform, void *native, const EGLint *attrib);
    EGLDisplay (*get_display)(EGLNativeDisplayType display);
    EGLBoolean (*initialize)(EGLDisplay dpy, EGLint *major, EGLint *minor);
    EGLBoolean (*terminate)(EGLDisplay dpy);
    EGLBoolean (*bind_api)(EGLenum api);
    EGLBoolean (*choose_config)(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs,
                                EGLint config_size, EGLint *num_config);
    EGLContext (*create_context)(EGLDisplay dpy, EGLConfig config, EGLContext share,
                                 const EGLint *attribs);
    EGLBoolean (*make_current)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
    EGLImageKHR (*create_image)(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                                EGLClientBuffer buffer, const EGLint *attribs);
    EGLBoolean (*destroy_image)(EGLDisplay dpy, EGLImageKHR image);
    EGLint (*get_error)(void);
    const char *(*query_string)(EGLDisplay dpy, EGLint name);
    __eglMustCastToProperFunctionPointerType (*get_proc_address)(const char *name);
} EglApi;

typedef struct GlApi {
    void (*GenTextures)(GLsizei n, GLuint *textures);
    void (*DeleteTextures)(GLsizei n, const GLuint *textures);
    void (*BindTexture)(GLenum target, GLuint texture);
    void (*TexParameteri)(GLenum target, GLenum pname, GLint param);
    void (*TexImage2D)(GLenum target, GLint level, GLint internal_format, GLsizei width,
                       GLsizei height, GLint border, GLenum format, GLenum type, const void *data);
    void (*GenerateMipmap)(GLenum target);
    void (*ActiveTexture)(GLenum texture);
    void (*PixelStorei)(GLenum pname, GLint param);
    void (*GenFramebuffers)(GLsizei n, GLuint *framebuffers);
    void (*DeleteFramebuffers)(GLsizei n, const GLuint *framebuffers);
    void (*BindFramebuffer)(GLenum target, GLuint framebuffer);
    void (*FramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                                 GLint level);
    GLenum (*CheckFramebufferStatus)(GLenum target);
    void (*BlitFramebuffer)(GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1, GLint dst_x0,
                            GLint dst_y0, GLint dst_x1, GLint dst_y1, GLbitfield mask, GLenum filter);
    void (*EGLImageTargetTexture2D)(GLenum target, GLeglImageOES image);
    GLuint (*CreateShader)(GLenum type);
    void (*ShaderSource)(GLuint shader, GLsizei count, const GLchar *const *string,
                         const GLint *length);
    void (*CompileShader)(GLuint shader);
    void (*GetShaderiv)(GLuint shader, GLenum pname, GLint *params);
    void (*GetShaderInfoLog)(GLuint shader, GLsizei buf_size, GLsizei *length, GLchar *info_log);
    void (*DeleteShader)(GLuint shader);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint program, GLuint shader);
    void (*LinkProgram)(GLuint program);
    void (*GetProgramiv)(GLuint program, GLenum pname, GLint *params);
    void (*GetProgramInfoLog)(GLuint program, GLsizei buf_size, GLsizei *length, GLchar *info_log);
    void (*UseProgram)(GLuint program);
    void (*DeleteProgram)(GLuint program);
    GLint (*GetUniformLocation)(GLuint program, const GLchar *name);
    void (*Uniform1i)(GLint location, GLint v0);
    void (*Uniform1f)(GLint location, GLfloat v0);
    void (*Uniform2f)(GLint location, GLfloat v0, GLfloat v1);
    void (*Uniform3f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
    void (*GenVertexArrays)(GLsizei n, GLuint *arrays);
    void (*DeleteVertexArrays)(GLsizei n, const GLuint *arrays);
    void (*BindVertexArray)(GLuint array);
    void (*GenBuffers)(GLsizei n, GLuint *buffers);
    void (*DeleteBuffers)(GLsizei n, const GLuint *buffers);
    void (*BindBuffer)(GLenum target, GLuint buffer);
    void (*BufferData)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
    void (*VertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                GLsizei stride, const void *pointer);
    void (*EnableVertexAttribArray)(GLuint index);
    void (*DrawArrays)(GLenum mode, GLint first, GLsizei count);
    void (*Viewport)(GLint x, GLint y, GLsizei width, GLsizei height);
    void (*Scissor)(GLint x, GLint y, GLsizei width, GLsizei height);
    void (*ClearColor)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    void (*Clear)(GLbitfield mask);
    void (*Enable)(GLenum cap);
    void (*Disable)(GLenum cap);
    void (*BlendFuncSeparate)(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha);
    void (*ReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                       void *pixels);
    void (*Finish)(void);
    GLenum (*GetError)(void);
} GlApi;

// --- shaders ---------------------------------------------------------------

// Shared by every fragment shader: the two interpolated coordinates, the output
// and the rounded rectangle they are all about.
static const char kFragmentHead[] =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec2 v_local;\n"
    "out vec4 o_colour;\n"
    "float sd_round_box(vec2 p, vec2 b, float r) {\n"
    "    vec2 q = abs(p) - b + vec2(r);\n"
    "    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
    "}\n";

static const char kVertexShader[] =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_ndc;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "layout(location = 2) in vec2 a_local;\n"
    "out vec2 v_uv;\n"
    "out vec2 v_local;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_local = a_local;\n"
    "    gl_Position = vec4(a_ndc, 0.0, 1.0);\n"
    "}\n";

static const char kFragmentBlit[] =
    "uniform sampler2D u_texture;\n"
    "void main() { o_colour = texture(u_texture, v_uv); }\n";

// A mask is one channel of coverage; the shadow it draws is black.
static const char kFragmentMask[] =
    "uniform sampler2D u_texture;\n"
    "void main() { o_colour = vec4(0.0, 0.0, 0.0, texture(u_texture, v_uv).r); }\n";

// The silhouette a shadow is blurred from.  It lands in a single-channel
// texture, so the coverage goes into red.
static const char kFragmentFill[] =
    "uniform vec2 u_half;\n"
    "uniform float u_radius;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "    float d = sd_round_box(v_local, u_half, u_radius);\n"
    "    o_colour = vec4(u_alpha * clamp(0.5 - d, 0.0, 1.0), 0.0, 0.0, 1.0);\n"
    "}\n";

static const char kFragmentBlur[] =
    "uniform sampler2D u_texture;\n"
    "uniform vec2 u_step;\n"
    "uniform float u_radius;\n"
    "void main() {\n"
    "    int reach = int(u_radius);\n"
    "    float sum = 0.0;\n"
    "    float count = 0.0;\n"
    "    for (int i = -reach; i <= reach; ++i) {\n"
    "        sum += texture(u_texture, v_uv + u_step * float(i)).r;\n"
    "        count += 1.0;\n"
    "    }\n"
    "    o_colour = vec4(sum / max(count, 1.0), 0.0, 0.0, 1.0);\n"
    "}\n";

static const char kFragmentImage[] =
    "uniform sampler2D u_texture;\n"
    "uniform vec2 u_half;\n"
    "uniform float u_radius;\n"
    "void main() {\n"
    "    vec4 c = texture(u_texture, v_uv);\n"
    "    float d = sd_round_box(v_local, u_half, u_radius);\n"
    "    float coverage = clamp(0.5 - d, 0.0, 1.0);\n"
    "    o_colour = vec4(c.b, c.g, c.r, c.a * coverage);\n"
    "}\n";

static const char kFragmentRim[] =
    "uniform vec2 u_half;\n"
    "uniform float u_radius;\n"
    "uniform float u_thickness;\n"
    "uniform vec3 u_colour;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "    float d = sd_round_box(v_local, u_half, u_radius);\n"
    "    float coverage = clamp(u_thickness * 0.5 - abs(d) + 0.5, 0.0, 1.0);\n"
    "    o_colour = vec4(u_colour, coverage * u_alpha);\n"
    "}\n";

// --- the context -----------------------------------------------------------

enum Program {
    PROGRAM_BLIT = 0,
    PROGRAM_MASK,
    PROGRAM_FILL,
    PROGRAM_BLUR,
    PROGRAM_IMAGE,
    PROGRAM_RIM,
    PROGRAM_COUNT
};

struct Texture {
    uint64_t id;
    GLuint texture;
    uint32_t width;
    uint32_t height;
    struct Texture *next;
};

struct Slot {
    int used;
    struct gbm_bo *bo;
    EGLImageKHR image;
    GLuint texture;
    GLuint framebuffer;
    vshot_fp16_buffer buffer;
};

struct vshot_fp16 {
    void *gbm_handle;
    void *egl_handle;
    GbmApi gbm;
    EglApi egl;
    GlApi gl;

    struct gbm_device *device;
    int node;
    EGLDisplay display;
    EGLContext context;

    uint32_t width;
    uint32_t height;
    GLuint compose_texture;
    GLuint compose_framebuffer;
    // The size of whatever is being drawn into right now.  It is almost always
    // the compose texture, but the shadow is built in a much smaller pair of
    // scratch textures, and the quad mapping has to be told which of the two it
    // is: mapping a scratch-sized quad with the compose's size puts it off the
    // edge of the scratch, which is a mask of nothing.
    uint32_t draw_width;
    uint32_t draw_height;
    // The small pair a shadow is blurred in, at the downscaled size.
    GLuint scratch[2];
    GLuint scratch_framebuffer[2];
    uint32_t scratch_width;
    uint32_t scratch_height;

    GLuint programs[PROGRAM_COUNT];
    GLint blur_radius_uniform;
    GLuint vertex_array;
    GLuint vertex_buffer;

    struct Texture *images;
    struct Texture *masks;
    struct Slot slots[FP16_SLOTS];
    char why[256];
};

static void fail(vshot_fp16 *ctx, const char *what) {
    snprintf(ctx->why, sizeof ctx->why, "%s", what);
}

static void failf(vshot_fp16 *ctx, const char *what, const char *detail) {
    snprintf(ctx->why, sizeof ctx->why, "%s: %s", what, detail);
}

static GLuint compile_shader(vshot_fp16 *ctx, const char *body, GLenum type) {
    GLuint shader = ctx->gl.CreateShader(type);
    const char *sources[2];
    int count = 0;
    if (type == GL_FRAGMENT_SHADER) {
        sources[count++] = kFragmentHead;
    }
    sources[count++] = body;
    ctx->gl.ShaderSource(shader, count, sources, NULL);
    ctx->gl.CompileShader(shader);
    GLint ok = 0;
    ctx->gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        ctx->gl.GetShaderInfoLog(shader, sizeof log - 1, NULL, log);
        failf(ctx, "a shader did not compile", log);
        ctx->gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(vshot_fp16 *ctx, const char *fragment) {
    GLuint vertex = compile_shader(ctx, kVertexShader, GL_VERTEX_SHADER);
    if (!vertex) {
        return 0;
    }
    GLuint fragment_shader = compile_shader(ctx, fragment, GL_FRAGMENT_SHADER);
    if (!fragment_shader) {
        ctx->gl.DeleteShader(vertex);
        return 0;
    }
    GLuint program = ctx->gl.CreateProgram();
    ctx->gl.AttachShader(program, vertex);
    ctx->gl.AttachShader(program, fragment_shader);
    ctx->gl.LinkProgram(program);
    GLint ok = 0;
    ctx->gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    ctx->gl.DeleteShader(vertex);
    ctx->gl.DeleteShader(fragment_shader);
    if (!ok) {
        char log[512] = {0};
        ctx->gl.GetProgramInfoLog(program, sizeof log - 1, NULL, log);
        failf(ctx, "a program did not link", log);
        ctx->gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

// --- a quad ----------------------------------------------------------------

// One draw is six vertices of (ndc, uv, local).  `local` is the position in
// device pixels relative to the shape's own centre, which is what the
// rounded-rectangle distance uses; growing a quad to give a stroke room does not
// move that centre.
//
// The vertical mapping is the one surprise here.  Everything above and below
// this function speaks the coordinates a pin is placed in: zero at the top of an
// output, growing downwards, which is what a layer surface's damage and a
// compositor's feedback are in.  OpenGL counts the other way -- row zero at the
// bottom of a framebuffer -- and, on a texture imported from a dma-buf, its row
// zero is the buffer's *first* row, which is the top of the image.  So the two
// cancel: drawing at a picture's row `y` with GL's own mapping is what lands it
// on that row of the buffer the compositor reads.  Compensating here instead --
// which is the obvious thing to write -- puts the whole picture on the screen
// upside down, and only for a pin far enough from the middle to notice.
static void quad(vshot_fp16 *ctx, float x, float y, float w, float h, float u0, float v0, float u1,
                 float v1, float centre_x, float centre_y) {
    const float view_w = (float)ctx->draw_width;
    const float view_h = (float)ctx->draw_height;
    const float xs[2] = {x, x + w};
    const float ys[2] = {y, y + h};
    const float us[2] = {u0, u1};
    const float vs[2] = {v0, v1};
    const int order[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
    float vertices[36];
    for (int i = 0; i < 6; ++i) {
        const int ix = order[i][0];
        const int iy = order[i][1];
        float *v = vertices + i * 6;
        v[0] = xs[ix] / view_w * 2.0f - 1.0f;
        v[1] = ys[iy] / view_h * 2.0f - 1.0f;
        v[2] = us[ix];
        v[3] = vs[iy];
        v[4] = xs[ix] - centre_x;
        v[5] = ys[iy] - centre_y;
    }
    ctx->gl.BindBuffer(GL_ARRAY_BUFFER, ctx->vertex_buffer);
    ctx->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof vertices, vertices, GL_STREAM_DRAW);
}

/// Makes `framebuffer` the draw target and remembers how big it is, which is
/// what [`quad`] maps its coordinates against.
static void bind_target(vshot_fp16 *ctx, GLuint framebuffer, uint32_t width, uint32_t height) {
    ctx->gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    ctx->gl.Viewport(0, 0, (GLsizei)width, (GLsizei)height);
    ctx->draw_width = width;
    ctx->draw_height = height;
}

static void draw_quad(vshot_fp16 *ctx, int program) {
    ctx->gl.UseProgram(ctx->programs[program]);
    ctx->gl.BindVertexArray(ctx->vertex_array);
    ctx->gl.DrawArrays(GL_TRIANGLES, 0, 6);
}

static struct Texture *find_texture(struct Texture *list, uint64_t id) {
    for (; list; list = list->next) {
        if (list->id == id) {
            return list;
        }
    }
    return NULL;
}

static void drop_texture(vshot_fp16 *ctx, struct Texture **list, uint64_t id) {
    struct Texture **link = list;
    while (*link) {
        if ((*link)->id == id) {
            struct Texture *dead = *link;
            *link = dead->next;
            ctx->gl.DeleteTextures(1, &dead->texture);
            free(dead);
            return;
        }
        link = &(*link)->next;
    }
}

static GLuint make_texture(vshot_fp16 *ctx, GLint internal, GLenum format, GLenum type,
                           uint32_t width, uint32_t height, const void *data) {
    GLuint texture = 0;
    ctx->gl.GenTextures(1, &texture);
    ctx->gl.BindTexture(GL_TEXTURE_2D, texture);
    ctx->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ctx->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ctx->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ctx->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ctx->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ctx->gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    ctx->gl.TexImage2D(GL_TEXTURE_2D, 0, internal, (GLsizei)width, (GLsizei)height, 0, format, type,
                       data);
    return texture;
}

static int framebuffer_for(vshot_fp16 *ctx, GLuint texture, GLuint *framebuffer) {
    ctx->gl.GenFramebuffers(1, framebuffer);
    ctx->gl.BindFramebuffer(GL_FRAMEBUFFER, *framebuffer);
    ctx->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    return ctx->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? 0 : -1;
}

// --- opening ---------------------------------------------------------------

#define TAKE(table, handle, member, name)                                                           \
    do {                                                                                            \
        (table).member = (void *)(uintptr_t)sym((handle), (name));                                  \
        if (!(table).member) {                                                                      \
            failf(ctx, "a library is missing an entry point", (name));                               \
            goto bad;                                                                               \
        }                                                                                           \
    } while (0)

// Fetches one OpenGL entry point: EGL first, then libGL, the way every EGL
// client does it -- those symbols are not in the link line.
#define GL_FUNCTION(ctx, member, name)                                                              \
    do {                                                                                            \
        void *found = (void *)(ctx)->egl.get_proc_address(name);                                     \
        if (!found) {                                                                               \
            static void *library = NULL;                                                            \
            if (!library) {                                                                         \
                static const char *names[] = {"libGL.so.1", "libGL.so", NULL};                       \
                library = try_open(names);                                                          \
            }                                                                                       \
            if (library) {                                                                          \
                found = sym(library, name);                                                         \
            }                                                                                       \
        }                                                                                           \
        (ctx)->gl.member = found;                                                                   \
        if (!(ctx)->gl.member) {                                                                    \
            failf(ctx, "the GL driver is missing an entry point", (name));                          \
            goto bad;                                                                               \
        }                                                                                           \
    } while (0)

vshot_fp16 *vshot_fp16_open(const char *render_node, char *why, size_t why_len) {
    vshot_fp16 *ctx = calloc(1, sizeof *ctx);
    if (!ctx) {
        if (why) {
            snprintf(why, why_len, "out of memory");
        }
        return NULL;
    }
    fail(ctx, "no reason recorded");

    static const char *gbm_names[] = {"libgbm.so.1", "libgbm.so", NULL};
    static const char *egl_names[] = {"libEGL.so.1", "libEGL.so", NULL};
    ctx->gbm_handle = try_open(gbm_names);
    ctx->egl_handle = try_open(egl_names);
    if (!ctx->gbm_handle || !ctx->egl_handle) {
        fail(ctx, "libgbm or libEGL is not installed");
        goto bad;
    }

    TAKE(ctx->gbm, ctx->gbm_handle, create_device, "gbm_create_device");
    TAKE(ctx->gbm, ctx->gbm_handle, device_destroy, "gbm_device_destroy");
    TAKE(ctx->gbm, ctx->gbm_handle, bo_create_with_modifiers, "gbm_bo_create_with_modifiers");
    TAKE(ctx->gbm, ctx->gbm_handle, bo_get_fd, "gbm_bo_get_fd");
    TAKE(ctx->gbm, ctx->gbm_handle, bo_get_stride_for_plane, "gbm_bo_get_stride_for_plane");
    TAKE(ctx->gbm, ctx->gbm_handle, bo_get_offset, "gbm_bo_get_offset");
    TAKE(ctx->gbm, ctx->gbm_handle, bo_get_modifier, "gbm_bo_get_modifier");
    TAKE(ctx->gbm, ctx->gbm_handle, bo_get_format, "gbm_bo_get_format");
    TAKE(ctx->gbm, ctx->gbm_handle, bo_get_plane_count, "gbm_bo_get_plane_count");
    TAKE(ctx->gbm, ctx->gbm_handle, bo_destroy, "gbm_bo_destroy");

    TAKE(ctx->egl, ctx->egl_handle, get_proc_address, "eglGetProcAddress");
    TAKE(ctx->egl, ctx->egl_handle, get_display, "eglGetDisplay");
    TAKE(ctx->egl, ctx->egl_handle, initialize, "eglInitialize");
    TAKE(ctx->egl, ctx->egl_handle, terminate, "eglTerminate");
    TAKE(ctx->egl, ctx->egl_handle, bind_api, "eglBindAPI");
    TAKE(ctx->egl, ctx->egl_handle, choose_config, "eglChooseConfig");
    TAKE(ctx->egl, ctx->egl_handle, create_context, "eglCreateContext");
    TAKE(ctx->egl, ctx->egl_handle, make_current, "eglMakeCurrent");
    TAKE(ctx->egl, ctx->egl_handle, get_error, "eglGetError");
    TAKE(ctx->egl, ctx->egl_handle, query_string, "eglQueryString");
    // Optional: an EGL implementation may not carry the platform extension.
    // It is an extension entry point, so it comes from `eglGetProcAddress` and
    // not from the library's exported names -- a GBM device is not something
    // the platform-less `eglGetDisplay` knows how to take.
    ctx->egl.get_platform_display = (EGLDisplay(*)(EGLenum, void *, const EGLint *))
        ctx->egl.get_proc_address("eglGetPlatformDisplayEXT");
    if (!ctx->egl.get_platform_display) {
        ctx->egl.get_platform_display = (EGLDisplay(*)(EGLenum, void *, const EGLint *))sym(
            ctx->egl_handle, "eglGetPlatformDisplayEXT");
    }

    GL_FUNCTION(ctx, GenTextures, "glGenTextures");
    GL_FUNCTION(ctx, DeleteTextures, "glDeleteTextures");
    GL_FUNCTION(ctx, BindTexture, "glBindTexture");
    GL_FUNCTION(ctx, TexParameteri, "glTexParameteri");
    GL_FUNCTION(ctx, TexImage2D, "glTexImage2D");
    GL_FUNCTION(ctx, GenerateMipmap, "glGenerateMipmap");
    GL_FUNCTION(ctx, ActiveTexture, "glActiveTexture");
    GL_FUNCTION(ctx, PixelStorei, "glPixelStorei");
    GL_FUNCTION(ctx, GenFramebuffers, "glGenFramebuffers");
    GL_FUNCTION(ctx, DeleteFramebuffers, "glDeleteFramebuffers");
    GL_FUNCTION(ctx, BindFramebuffer, "glBindFramebuffer");
    GL_FUNCTION(ctx, FramebufferTexture2D, "glFramebufferTexture2D");
    GL_FUNCTION(ctx, CheckFramebufferStatus, "glCheckFramebufferStatus");
    GL_FUNCTION(ctx, BlitFramebuffer, "glBlitFramebuffer");
    GL_FUNCTION(ctx, EGLImageTargetTexture2D, "glEGLImageTargetTexture2DOES");
    GL_FUNCTION(ctx, CreateShader, "glCreateShader");
    GL_FUNCTION(ctx, ShaderSource, "glShaderSource");
    GL_FUNCTION(ctx, CompileShader, "glCompileShader");
    GL_FUNCTION(ctx, GetShaderiv, "glGetShaderiv");
    GL_FUNCTION(ctx, GetShaderInfoLog, "glGetShaderInfoLog");
    GL_FUNCTION(ctx, DeleteShader, "glDeleteShader");
    GL_FUNCTION(ctx, CreateProgram, "glCreateProgram");
    GL_FUNCTION(ctx, AttachShader, "glAttachShader");
    GL_FUNCTION(ctx, LinkProgram, "glLinkProgram");
    GL_FUNCTION(ctx, GetProgramiv, "glGetProgramiv");
    GL_FUNCTION(ctx, GetProgramInfoLog, "glGetProgramInfoLog");
    GL_FUNCTION(ctx, UseProgram, "glUseProgram");
    GL_FUNCTION(ctx, DeleteProgram, "glDeleteProgram");
    GL_FUNCTION(ctx, GetUniformLocation, "glGetUniformLocation");
    GL_FUNCTION(ctx, Uniform1i, "glUniform1i");
    GL_FUNCTION(ctx, Uniform1f, "glUniform1f");
    GL_FUNCTION(ctx, Uniform2f, "glUniform2f");
    GL_FUNCTION(ctx, Uniform3f, "glUniform3f");
    GL_FUNCTION(ctx, GenVertexArrays, "glGenVertexArrays");
    GL_FUNCTION(ctx, DeleteVertexArrays, "glDeleteVertexArrays");
    GL_FUNCTION(ctx, BindVertexArray, "glBindVertexArray");
    GL_FUNCTION(ctx, GenBuffers, "glGenBuffers");
    GL_FUNCTION(ctx, DeleteBuffers, "glDeleteBuffers");
    GL_FUNCTION(ctx, BindBuffer, "glBindBuffer");
    GL_FUNCTION(ctx, BufferData, "glBufferData");
    GL_FUNCTION(ctx, VertexAttribPointer, "glVertexAttribPointer");
    GL_FUNCTION(ctx, EnableVertexAttribArray, "glEnableVertexAttribArray");
    GL_FUNCTION(ctx, DrawArrays, "glDrawArrays");
    GL_FUNCTION(ctx, Viewport, "glViewport");
    GL_FUNCTION(ctx, Scissor, "glScissor");
    GL_FUNCTION(ctx, ClearColor, "glClearColor");
    GL_FUNCTION(ctx, Clear, "glClear");
    GL_FUNCTION(ctx, Enable, "glEnable");
    GL_FUNCTION(ctx, Disable, "glDisable");
    GL_FUNCTION(ctx, BlendFuncSeparate, "glBlendFuncSeparate");
    GL_FUNCTION(ctx, ReadPixels, "glReadPixels");
    GL_FUNCTION(ctx, Finish, "glFinish");
    GL_FUNCTION(ctx, GetError, "glGetError");

    // The device keeps the descriptor it was made from -- closing it here would
    // take the allocator's channel with it -- so it is held until the context
    // goes.
    ctx->node = open(render_node, O_RDWR | O_CLOEXEC);
    if (ctx->node < 0) {
        failf(ctx, "the render node cannot be opened", render_node);
        goto bad;
    }
    ctx->device = ctx->gbm.create_device(ctx->node);
    if (!ctx->device) {
        fail(ctx, "gbm could not make a device for the render node");
        goto bad;
    }
    if (ctx->egl.get_platform_display) {
        ctx->display = ctx->egl.get_platform_display(EGL_PLATFORM_GBM_KHR, ctx->device, NULL);
    }
    if (ctx->display == EGL_NO_DISPLAY) {
        ctx->display = ctx->egl.get_display((EGLNativeDisplayType)ctx->device);
    }
    if (ctx->display == EGL_NO_DISPLAY) {
        fail(ctx, "EGL has no display for the render node");
        goto bad;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (!ctx->egl.initialize(ctx->display, &major, &minor)) {
        fail(ctx, "eglInitialize failed");
        goto bad;
    }
    if (!ctx->egl.bind_api(EGL_OPENGL_API)) {
        fail(ctx, "this EGL has no OpenGL API");
        goto bad;
    }
    // The image entry points are extensions, so they are not exported by name:
    // EGL resolves them once it is initialized, and a 1.5 core name is the
    // fallback for a driver that only carries the unsuffixed form.
    ctx->egl.create_image = (EGLImageKHR(*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer,
                                          const EGLint *))ctx->egl.get_proc_address(
        "eglCreateImageKHR");
    if (!ctx->egl.create_image) {
        ctx->egl.create_image = (EGLImageKHR(*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer,
                                               const EGLint *))ctx->egl.get_proc_address(
            "eglCreateImage");
    }
    ctx->egl.destroy_image = (EGLBoolean(*)(EGLDisplay, EGLImageKHR))ctx->egl.get_proc_address(
        "eglDestroyImageKHR");
    if (!ctx->egl.destroy_image) {
        ctx->egl.destroy_image =
            (EGLBoolean(*)(EGLDisplay, EGLImageKHR))ctx->egl.get_proc_address("eglDestroyImage");
    }
    if (!ctx->egl.create_image || !ctx->egl.destroy_image) {
        fail(ctx, "this EGL cannot import a dma-buf as an image");
        goto bad;
    }
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                        EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig config;
    EGLint configs = 0;
    if (!ctx->egl.choose_config(ctx->display, config_attributes, &config, 1, &configs) ||
        configs < 1) {
        fail(ctx, "EGL has no config for an OpenGL context");
        goto bad;
    }
    ctx->context = ctx->egl.create_context(ctx->display, config, EGL_NO_CONTEXT, NULL);
    if (ctx->context == EGL_NO_CONTEXT) {
        fail(ctx, "eglCreateContext failed");
        goto bad;
    }
    // Surfaceless: everything is drawn into framebuffers of this side's own.
    if (!ctx->egl.make_current(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context)) {
        fail(ctx, "eglMakeCurrent failed");
        goto bad;
    }

    {
        static const char *bodies[PROGRAM_COUNT] = {kFragmentBlit, kFragmentMask, kFragmentFill,
                                                    kFragmentBlur, kFragmentImage, kFragmentRim};
        for (int i = 0; i < PROGRAM_COUNT; i++) {
            ctx->programs[i] = link_program(ctx, bodies[i]);
            if (!ctx->programs[i]) {
                goto bad;
            }
        }
    }
    ctx->blur_radius_uniform = ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_BLUR], "u_radius");

    ctx->gl.GenVertexArrays(1, &ctx->vertex_array);
    ctx->gl.GenBuffers(1, &ctx->vertex_buffer);
    ctx->gl.BindVertexArray(ctx->vertex_array);
    ctx->gl.BindBuffer(GL_ARRAY_BUFFER, ctx->vertex_buffer);
    const GLsizei stride = 6 * (GLsizei)sizeof(float);
    ctx->gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (const void *)0);
    ctx->gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                                (const void *)(2 * sizeof(float)));
    ctx->gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                                (const void *)(4 * sizeof(float)));
    ctx->gl.EnableVertexAttribArray(0);
    ctx->gl.EnableVertexAttribArray(1);
    ctx->gl.EnableVertexAttribArray(2);
    ctx->gl.Disable(GL_DEPTH_TEST);
    ctx->gl.Disable(GL_CULL_FACE);
    // Straight alpha in, straight alpha out: the picture is not premultiplied
    // until the compositor reads it.
    ctx->gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return ctx;

bad:
    if (why) {
        snprintf(why, why_len, "%s", ctx->why);
    }
    vshot_fp16_close(ctx);
    return NULL;
}

void vshot_fp16_close(vshot_fp16 *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->context != EGL_NO_CONTEXT && ctx->display != EGL_NO_DISPLAY) {
        for (int i = 0; i < FP16_SLOTS; i++) {
            if (ctx->slots[i].used) {
                vshot_fp16_free_buffer(ctx, &ctx->slots[i].buffer);
            }
        }
        while (ctx->images) {
            struct Texture *next = ctx->images->next;
            ctx->gl.DeleteTextures(1, &ctx->images->texture);
            free(ctx->images);
            ctx->images = next;
        }
        while (ctx->masks) {
            struct Texture *next = ctx->masks->next;
            ctx->gl.DeleteTextures(1, &ctx->masks->texture);
            free(ctx->masks);
            ctx->masks = next;
        }
        if (ctx->compose_framebuffer) {
            ctx->gl.DeleteFramebuffers(1, &ctx->compose_framebuffer);
        }
        if (ctx->compose_texture) {
            ctx->gl.DeleteTextures(1, &ctx->compose_texture);
        }
        for (int i = 0; i < 2; i++) {
            if (ctx->scratch_framebuffer[i]) {
                ctx->gl.DeleteFramebuffers(1, &ctx->scratch_framebuffer[i]);
            }
            if (ctx->scratch[i]) {
                ctx->gl.DeleteTextures(1, &ctx->scratch[i]);
            }
        }
        for (int i = 0; i < PROGRAM_COUNT; i++) {
            if (ctx->programs[i]) {
                ctx->gl.DeleteProgram(ctx->programs[i]);
            }
        }
        if (ctx->vertex_buffer) {
            ctx->gl.DeleteBuffers(1, &ctx->vertex_buffer);
        }
        if (ctx->vertex_array) {
            ctx->gl.DeleteVertexArrays(1, &ctx->vertex_array);
        }
        ctx->egl.make_current(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        ctx->egl.terminate(ctx->display);
    }
    if (ctx->device) {
        ctx->gbm.device_destroy(ctx->device);
    }
    if (ctx->node > 0) {
        close(ctx->node);
    }
    free(ctx);
}

// --- buffers ---------------------------------------------------------------

int vshot_fp16_alloc(vshot_fp16 *ctx, uint32_t width, uint32_t height, uint32_t format,
                     uint64_t modifier, vshot_fp16_buffer *out, char *why, size_t why_len) {
    struct Slot *target = NULL;
    if (!ctx || !out) {
        return -1;
    }
    fail(ctx, "no reason recorded");
    for (int i = 0; i < FP16_SLOTS; i++) {
        if (!ctx->slots[i].used) {
            target = &ctx->slots[i];
            break;
        }
    }
    if (!target) {
        fail(ctx, "no picture buffer is free");
        goto bad;
    }
    const uint64_t modifiers[1] = {modifier};
    target->bo = ctx->gbm.bo_create_with_modifiers(ctx->device, width, height, format, modifiers, 1);
    if (!target->bo) {
        fail(ctx, "gbm could not allocate the picture buffer");
        goto bad;
    }
    if (ctx->gbm.bo_get_modifier(target->bo) != modifier) {
        fail(ctx, "gbm allocated with a different modifier than asked for");
        goto bad;
    }
    const EGLint attributes[] = {
        EGL_WIDTH,
        (EGLint)width,
        EGL_HEIGHT,
        (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT,
        (EGLint)format,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        ctx->gbm.bo_get_fd(target->bo),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        (EGLint)ctx->gbm.bo_get_offset(target->bo, 0),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        (EGLint)ctx->gbm.bo_get_stride_for_plane(target->bo, 0),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        (EGLint)(modifier & 0xffffffffu),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        (EGLint)(modifier >> 32),
        EGL_NONE};
    target->image =
        ctx->egl.create_image(ctx->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    if (target->image == EGL_NO_IMAGE_KHR) {
        fail(ctx, "EGL could not import the picture buffer");
        goto bad;
    }
    ctx->gl.GenTextures(1, &target->texture);
    ctx->gl.BindTexture(GL_TEXTURE_2D, target->texture);
    ctx->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    ctx->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ctx->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ctx->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ctx->gl.EGLImageTargetTexture2D(GL_TEXTURE_2D, target->image);
    if (framebuffer_for(ctx, target->texture, &target->framebuffer) != 0) {
        fail(ctx, "the picture buffer cannot be drawn into");
        goto bad;
    }
    target->used = 1;
    target->buffer.slot = (int)(target - ctx->slots);
    target->buffer.fd = ctx->gbm.bo_get_fd(target->bo);
    target->buffer.stride = ctx->gbm.bo_get_stride_for_plane(target->bo, 0);
    target->buffer.offset = ctx->gbm.bo_get_offset(target->bo, 0);
    target->buffer.modifier = modifier;
    target->buffer.format = format;
    *out = target->buffer;
    return 0;

bad:
    if (target) {
        if (target->buffer.fd > 0) {
            close(target->buffer.fd);
            target->buffer.fd = 0;
        }
        if (target->framebuffer) {
            ctx->gl.DeleteFramebuffers(1, &target->framebuffer);
            target->framebuffer = 0;
        }
        if (target->texture) {
            ctx->gl.DeleteTextures(1, &target->texture);
            target->texture = 0;
        }
        if (target->image != EGL_NO_IMAGE_KHR) {
            ctx->egl.destroy_image(ctx->display, target->image);
            target->image = EGL_NO_IMAGE_KHR;
        }
        if (target->bo) {
            ctx->gbm.bo_destroy(target->bo);
            target->bo = NULL;
        }
        target->used = 0;
    }
    if (why) {
        snprintf(why, why_len, "%s", ctx->why);
    }
    return -1;
}

void vshot_fp16_free_buffer(vshot_fp16 *ctx, vshot_fp16_buffer *buffer) {
    if (!ctx || !buffer || buffer->slot < 0 || buffer->slot >= FP16_SLOTS) {
        return;
    }
    struct Slot *slot = &ctx->slots[buffer->slot];
    if (!slot->used) {
        return;
    }
    if (slot->buffer.fd > 0) {
        close(slot->buffer.fd);
    }
    if (slot->framebuffer) {
        ctx->gl.DeleteFramebuffers(1, &slot->framebuffer);
    }
    if (slot->texture) {
        ctx->gl.DeleteTextures(1, &slot->texture);
    }
    if (slot->image != EGL_NO_IMAGE_KHR) {
        ctx->egl.destroy_image(ctx->display, slot->image);
    }
    if (slot->bo) {
        ctx->gbm.bo_destroy(slot->bo);
    }
    memset(slot, 0, sizeof *slot);
    memset(buffer, 0, sizeof *buffer);
    buffer->slot = -1;
}

// --- pictures --------------------------------------------------------------

int vshot_fp16_begin(vshot_fp16 *ctx, uint32_t width, uint32_t height) {
    if (!ctx || width == 0 || height == 0) {
        return -1;
    }
    if (ctx->width == width && ctx->height == height && ctx->compose_texture) {
        bind_target(ctx, ctx->compose_framebuffer, width, height);
        return 0;
    }
    if (ctx->compose_framebuffer) {
        ctx->gl.DeleteFramebuffers(1, &ctx->compose_framebuffer);
        ctx->compose_framebuffer = 0;
    }
    if (ctx->compose_texture) {
        ctx->gl.DeleteTextures(1, &ctx->compose_texture);
        ctx->compose_texture = 0;
    }
    ctx->width = width;
    ctx->height = height;
    ctx->compose_texture =
        make_texture(ctx, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, width, height, NULL);
    if (!ctx->compose_texture) {
        fail(ctx, "the compose texture could not be made");
        return -1;
    }
    if (framebuffer_for(ctx, ctx->compose_texture, &ctx->compose_framebuffer) != 0) {
        fail(ctx, "the compose framebuffer is incomplete");
        return -1;
    }
    // Nothing is on screen yet: the picture starts fully transparent.
    bind_target(ctx, ctx->compose_framebuffer, width, height);
    ctx->gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    ctx->gl.Clear(GL_COLOR_BUFFER_BIT);
    return 0;
}

void vshot_fp16_clear(vshot_fp16 *ctx, int x, int y, int width, int height) {
    if (!ctx || width <= 0 || height <= 0) {
        return;
    }
    bind_target(ctx, ctx->compose_framebuffer, ctx->width, ctx->height);
    ctx->gl.Enable(GL_SCISSOR_TEST);
    ctx->gl.Scissor(x, y, width, height);
    ctx->gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    ctx->gl.Clear(GL_COLOR_BUFFER_BIT);
    ctx->gl.Disable(GL_SCISSOR_TEST);
}

int vshot_fp16_image(vshot_fp16 *ctx, uint64_t id, const uint32_t *words, uint32_t width,
                     uint32_t height) {
    if (!ctx || !words || width == 0 || height == 0) {
        return -1;
    }
    struct Texture *known = find_texture(ctx->images, id);
    if (known && known->width == width && known->height == height) {
        return 0;
    }
    drop_texture(ctx, &ctx->images, id);
    struct Texture *texture = calloc(1, sizeof *texture);
    if (!texture) {
        fail(ctx, "out of memory");
        return -1;
    }
    texture->texture = make_texture(ctx, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                                    width, height, words);
    if (!texture->texture) {
        free(texture);
        fail(ctx, "the pin's pixels could not be uploaded");
        return -1;
    }
    // A pin is shown at its own size most of the time, but a 4K capture pinned
    // small is a long way down and wants the mipmaps rather than one bilinear
    // tap.  The pixels never change, so this happens once.
    ctx->gl.BindTexture(GL_TEXTURE_2D, texture->texture);
    ctx->gl.GenerateMipmap(GL_TEXTURE_2D);
    ctx->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    texture->id = id;
    texture->width = width;
    texture->height = height;
    texture->next = ctx->images;
    ctx->images = texture;
    return 0;
}

static int reserve_scratch(vshot_fp16 *ctx, uint32_t width, uint32_t height) {
    if (ctx->scratch[0] && ctx->scratch_width == width && ctx->scratch_height == height) {
        return 0;
    }
    for (int i = 0; i < 2; i++) {
        if (ctx->scratch_framebuffer[i]) {
            ctx->gl.DeleteFramebuffers(1, &ctx->scratch_framebuffer[i]);
            ctx->scratch_framebuffer[i] = 0;
        }
        if (ctx->scratch[i]) {
            ctx->gl.DeleteTextures(1, &ctx->scratch[i]);
            ctx->scratch[i] = 0;
        }
    }
    ctx->scratch_width = width;
    ctx->scratch_height = height;
    for (int i = 0; i < 2; i++) {
        ctx->scratch[i] = make_texture(ctx, GL_R8, GL_RED, GL_UNSIGNED_BYTE, width, height, NULL);
        if (!ctx->scratch[i]) {
            fail(ctx, "the shadow's own buffers could not be made");
            return -1;
        }
        if (framebuffer_for(ctx, ctx->scratch[i], &ctx->scratch_framebuffer[i]) != 0) {
            fail(ctx, "the shadow's framebuffer is incomplete");
            return -1;
        }
    }
    return 0;
}

int vshot_fp16_mask(vshot_fp16 *ctx, uint64_t id, int width, int height, int radius, int spread,
                    int offset, int opacity) {
    if (!ctx || width <= 0 || height <= 0 || spread <= 0 || opacity <= 0) {
        return -1;
    }
    // A mask already kept under this id is the mask this shape wants: the caller
    // is the one that decides when two shapes are the same.
    if (find_texture(ctx->masks, id)) {
        return 0;
    }
    drop_texture(ctx, &ctx->masks, id);

    // The box the blur needs: the shape grown by the reach on every side, the
    // silhouette dropped inside it by the offset -- so the shadow hangs below
    // the pin without the pin's own rect moving.
    const int box_width = width + 2 * spread;
    const int box_height = height + 2 * spread;
    const int small_width = box_width / SHADOW_DOWNSCALE > 0 ? box_width / SHADOW_DOWNSCALE : 1;
    const int small_height = box_height / SHADOW_DOWNSCALE > 0 ? box_height / SHADOW_DOWNSCALE : 1;
    if (reserve_scratch(ctx, (uint32_t)small_width, (uint32_t)small_height) != 0) {
        return -1;
    }
    // One small pixel of blur is the default shadow; a wider reach blurs wider.
    double wanted = (double)spread / (4.0 * SHADOW_DOWNSCALE);
    if (wanted < 1.0) {
        wanted = 1.0;
    }
    const float blur_radius = (float)floor(wanted + 0.5);

    // The silhouette, at the downscaled size: everything about it divides down
    // with the box.  Blending is off: this pass replaces what it draws.
    const float scale = 1.0f / (float)SHADOW_DOWNSCALE;
    const float shape_width = (float)width * scale;
    const float shape_height = (float)height * scale;
    const float shape_x = (float)spread * scale;
    const float shape_y = (float)(spread + offset) * scale;
    float scaled_radius = (float)radius * scale;
    const float most = 0.5f * (shape_width < shape_height ? shape_width : shape_height);
    if (scaled_radius > most) {
        scaled_radius = most;
    }
    int source = 0;
    bind_target(ctx, ctx->scratch_framebuffer[source], (uint32_t)small_width,
                (uint32_t)small_height);
    ctx->gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    ctx->gl.Clear(GL_COLOR_BUFFER_BIT);
    ctx->gl.UseProgram(ctx->programs[PROGRAM_FILL]);
    ctx->gl.Uniform2f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_FILL], "u_half"),
                      shape_width * 0.5f, shape_height * 0.5f);
    ctx->gl.Uniform1f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_FILL], "u_radius"),
                      scaled_radius);
    ctx->gl.Uniform1f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_FILL], "u_alpha"),
                      (float)opacity / 255.0f);
    quad(ctx, shape_x, shape_y, shape_width, shape_height, 0.0f, 0.0f, 0.0f, 0.0f,
         shape_x + shape_width * 0.5f, shape_y + shape_height * 0.5f);
    draw_quad(ctx, PROGRAM_FILL);

    // Two box passes, the way the SDR shadow is blurred: one pass leaves a hard
    // ramp, and the second turns it into a falloff.  Each pass reads the texture
    // the last one wrote.
    const float step_x = 1.0f / (float)small_width;
    const float step_y = 1.0f / (float)small_height;
    for (int pass = 0; pass < SHADOW_PASSES; pass++) {
        for (int axis = 0; axis < 2; axis++) {
            const int to = 1 - source;
            bind_target(ctx, ctx->scratch_framebuffer[to], (uint32_t)small_width,
                        (uint32_t)small_height);
            ctx->gl.UseProgram(ctx->programs[PROGRAM_BLUR]);
            ctx->gl.ActiveTexture(GL_TEXTURE0);
            ctx->gl.BindTexture(GL_TEXTURE_2D, ctx->scratch[source]);
            ctx->gl.Uniform1i(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_BLUR], "u_texture"),
                              0);
            ctx->gl.Uniform2f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_BLUR], "u_step"),
                              axis == 0 ? step_x : 0.0f, axis == 0 ? 0.0f : step_y);
            ctx->gl.Uniform1f(ctx->blur_radius_uniform, blur_radius);
            quad(ctx, 0.0f, 0.0f, (float)small_width, (float)small_height, 0.0f, 0.0f, 1.0f, 1.0f,
                 0.0f, 0.0f);
            draw_quad(ctx, PROGRAM_BLUR);
            source = to;
        }
    }

    // The finished mask, kept under the caller's id at its own small size: it is
    // drawn scaled up, and that upscale is where the softness comes from.
    struct Texture *texture = calloc(1, sizeof *texture);
    if (!texture) {
        fail(ctx, "out of memory");
        bind_target(ctx, ctx->compose_framebuffer, ctx->width, ctx->height);
        return -1;
    }
    texture->texture = make_texture(ctx, GL_R8, GL_RED, GL_UNSIGNED_BYTE, (uint32_t)small_width,
                                    (uint32_t)small_height, NULL);
    if (!texture->texture) {
        free(texture);
        fail(ctx, "the shadow mask could not be kept");
        bind_target(ctx, ctx->compose_framebuffer, ctx->width, ctx->height);
        return -1;
    }
    {
        GLuint copy = 0;
        if (framebuffer_for(ctx, texture->texture, &copy) != 0) {
            ctx->gl.DeleteTextures(1, &texture->texture);
            free(texture);
            fail(ctx, "the shadow mask could not be kept");
            bind_target(ctx, ctx->compose_framebuffer, ctx->width, ctx->height);
            return -1;
        }
        bind_target(ctx, copy, (uint32_t)small_width, (uint32_t)small_height);
        ctx->gl.UseProgram(ctx->programs[PROGRAM_BLIT]);
        ctx->gl.ActiveTexture(GL_TEXTURE0);
        ctx->gl.BindTexture(GL_TEXTURE_2D, ctx->scratch[source]);
        ctx->gl.Uniform1i(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_BLIT], "u_texture"), 0);
        quad(ctx, 0.0f, 0.0f, (float)small_width, (float)small_height, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
             0.0f);
        draw_quad(ctx, PROGRAM_BLIT);
        ctx->gl.DeleteFramebuffers(1, &copy);
    }
    texture->id = id;
    texture->width = (uint32_t)small_width;
    texture->height = (uint32_t)small_height;
    texture->next = ctx->masks;
    ctx->masks = texture;
    bind_target(ctx, ctx->compose_framebuffer, ctx->width, ctx->height);
    return 0;
}

void vshot_fp16_drop(vshot_fp16 *ctx, uint64_t id) {
    if (!ctx) {
        return;
    }
    drop_texture(ctx, &ctx->images, id);
    drop_texture(ctx, &ctx->masks, id);
}

int vshot_fp16_draw_shadow(vshot_fp16 *ctx, uint64_t mask_id, int x, int y, int width, int height) {
    struct Texture *mask = ctx ? find_texture(ctx->masks, mask_id) : NULL;
    if (!mask || width <= 0 || height <= 0) {
        return -1;
    }
    bind_target(ctx, ctx->compose_framebuffer, ctx->width, ctx->height);
    ctx->gl.Enable(GL_BLEND);
    ctx->gl.UseProgram(ctx->programs[PROGRAM_MASK]);
    ctx->gl.ActiveTexture(GL_TEXTURE0);
    ctx->gl.BindTexture(GL_TEXTURE_2D, mask->texture);
    ctx->gl.Uniform1i(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_MASK], "u_texture"), 0);
    quad(ctx, (float)x, (float)y, (float)width, (float)height, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);
    draw_quad(ctx, PROGRAM_MASK);
    ctx->gl.Disable(GL_BLEND);
    return 0;
}

int vshot_fp16_draw_image(vshot_fp16 *ctx, uint64_t image_id, int x, int y, int width, int height,
                          int radius) {
    struct Texture *image = ctx ? find_texture(ctx->images, image_id) : NULL;
    if (!image || width <= 0 || height <= 0) {
        return -1;
    }
    float round = (float)radius;
    const float most = 0.5f * (float)(width < height ? width : height);
    if (round > most) {
        round = most;
    }
    bind_target(ctx, ctx->compose_framebuffer, ctx->width, ctx->height);
    ctx->gl.Enable(GL_BLEND);
    ctx->gl.UseProgram(ctx->programs[PROGRAM_IMAGE]);
    ctx->gl.ActiveTexture(GL_TEXTURE0);
    ctx->gl.BindTexture(GL_TEXTURE_2D, image->texture);
    ctx->gl.Uniform1i(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_IMAGE], "u_texture"), 0);
    ctx->gl.Uniform2f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_IMAGE], "u_half"),
                      (float)width * 0.5f, (float)height * 0.5f);
    ctx->gl.Uniform1f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_IMAGE], "u_radius"), round);
    quad(ctx, (float)x, (float)y, (float)width, (float)height, 0.0f, 0.0f, 1.0f, 1.0f,
         (float)x + (float)width * 0.5f, (float)y + (float)height * 0.5f);
    draw_quad(ctx, PROGRAM_IMAGE);
    ctx->gl.Disable(GL_BLEND);
    return 0;
}

int vshot_fp16_draw_rim(vshot_fp16 *ctx, int x, int y, int width, int height, int radius,
                        int thickness, float red, float green, float blue, float alpha) {
    if (!ctx || width <= 0 || height <= 0 || thickness <= 0) {
        return -1;
    }
    float round = (float)radius;
    const float most = 0.5f * (float)(width < height ? width : height);
    if (round > most) {
        round = most;
    }
    // The stroke is centred on the edge, so half of it falls outside the pin and
    // the quad has to reach past it.
    const float pad = (float)thickness * 0.5f + 1.0f;
    bind_target(ctx, ctx->compose_framebuffer, ctx->width, ctx->height);
    ctx->gl.Enable(GL_BLEND);
    ctx->gl.UseProgram(ctx->programs[PROGRAM_RIM]);
    ctx->gl.Uniform2f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_RIM], "u_half"),
                      (float)width * 0.5f, (float)height * 0.5f);
    ctx->gl.Uniform1f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_RIM], "u_radius"), round);
    ctx->gl.Uniform1f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_RIM], "u_thickness"),
                      (float)thickness);
    ctx->gl.Uniform3f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_RIM], "u_colour"), red,
                      green, blue);
    ctx->gl.Uniform1f(ctx->gl.GetUniformLocation(ctx->programs[PROGRAM_RIM], "u_alpha"), alpha);
    quad(ctx, (float)x - pad, (float)y - pad, (float)width + 2.0f * pad, (float)height + 2.0f * pad,
         0.0f, 0.0f, 0.0f, 0.0f, (float)x + (float)width * 0.5f, (float)y + (float)height * 0.5f);
    draw_quad(ctx, PROGRAM_RIM);
    ctx->gl.Disable(GL_BLEND);
    return 0;
}

int vshot_fp16_present(vshot_fp16 *ctx, int slot, int x, int y, int width, int height) {
    if (!ctx || slot < 0 || slot >= FP16_SLOTS || !ctx->slots[slot].used || width <= 0 ||
        height <= 0) {
        return -1;
    }
    // The picture and the buffer agree row for row: the drawing side maps a
    // picture's rows onto OpenGL's the way the image import does, so a plain
    // copy of the region that changed is what lands it on the right rows.
    ctx->gl.BindFramebuffer(GL_READ_FRAMEBUFFER, ctx->compose_framebuffer);
    ctx->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->slots[slot].framebuffer);
    ctx->gl.BlitFramebuffer(x, y, x + width, y + height, x, y, x + width, y + height,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    ctx->gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->compose_framebuffer);
    ctx->draw_width = ctx->width;
    ctx->draw_height = ctx->height;
    // The compositor reads the buffer from the same device, through an implicit
    // fence; finishing is what gives that fence something to wait for.
    ctx->gl.Finish();
    if (getenv("VSHOT_FP16_DEBUG")) {
        // A coarse scan of the whole compose and of the buffer it was copied
        // into: where the brightest pixel is, and how opaque the two are.  The
        // points are the ones a caller can reason about whatever its pin's rect.
        float compose[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float buffer[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float best_compose = 0.0f;
        float best_buffer = 0.0f;
        int best_x = 0;
        int best_y = 0;
        for (uint32_t sy = 2; sy < ctx->height; sy += 64) {
            for (uint32_t sx = 2; sx < ctx->width; sx += 64) {
                float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                ctx->gl.BindFramebuffer(GL_READ_FRAMEBUFFER, ctx->compose_framebuffer);
                ctx->gl.ReadPixels((GLint)sx, (GLint)sy, 1, 1, GL_RGBA, GL_FLOAT, value);
                const float level = value[3] > 0.01f ? (value[0] > value[1] ? value[0] : value[1])
                                                     : 0.0f;
                if (level > best_compose) {
                    best_compose = level;
                    best_x = (int)sx;
                    best_y = (int)sy;
                }
            }
        }
        ctx->gl.BindFramebuffer(GL_READ_FRAMEBUFFER, ctx->compose_framebuffer);
        ctx->gl.ReadPixels(best_x, best_y, 1, 1, GL_RGBA, GL_FLOAT, compose);
        ctx->gl.BindFramebuffer(GL_READ_FRAMEBUFFER, ctx->slots[slot].framebuffer);
        ctx->gl.ReadPixels(best_x, best_y, 1, 1, GL_RGBA, GL_FLOAT, buffer);
        float best_alpha = 0.0f;
        for (uint32_t sy = 2; sy < ctx->height; sy += 64) {
            for (uint32_t sx = 2; sx < ctx->width; sx += 64) {
                float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                ctx->gl.ReadPixels((GLint)sx, (GLint)sy, 1, 1, GL_RGBA, GL_FLOAT, value);
                if (value[0] > best_buffer) {
                    best_buffer = value[0];
                }
                if (value[3] > best_alpha && value[3] < 0.99f) {
                    best_alpha = value[3];
                }
            }
        }
        ctx->gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->compose_framebuffer);
        fprintf(stderr,
                "vshot: fp16 present %d,%d %dx%d: compose brightest (%.3f %.3f %.3f %.3f) at "
                "%d,%d, same place in the buffer (%.3f %.3f %.3f %.3f), buffer max %.3f, "
                "partial alpha up to %.3f, gl "
                "error 0x%x\n",
                x, y, width, height, compose[0], compose[1], compose[2], compose[3], best_x,
                best_y, buffer[0], buffer[1], buffer[2], buffer[3], best_buffer, best_alpha,
                ctx->gl.GetError());
    }
    return 0;
}

#endif // VSHOT_FP16_HEADERS
