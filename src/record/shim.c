// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// vshot's libavcodec encoder shim.
//
// The recording encoder runs on ffmpeg's libavcodec, the way wf-recorder
// does it.  Why not libva directly?  Measured on this machine (mesa-git
// 26.3.0-devel, radeonsi): every direct-libva shape we tried — reusable
// coded buffer, per-frame coded buffer, vaSyncSurface, vaSyncBuffer,
// single or threaded conversion — segfaults inside vaEndPicture's
// radeonsi_drv_video feedback callback (`mov 0xb0(%rdi),%rax` with
// rdi = NULL) after 30-60 frames.  libavcodec's h264_vaapi path runs the
// same hardware through the same driver for hundreds of frames without a
// fault, so the encoder delegates VAAPI management to libavcodec instead
// of talking to libva itself.
//
// Two frame inputs are supported:
//
//   * `send` (software): one tightly packed RGBA buffer per frame; the
//     shim converts to NV12 on the CPU and uploads through libavcodec's
//     hwframe_transfer_data.  This is the compatibility path — it works
//     wherever the screenshots work, including KWin sessions and
//     compositors without linux-dmabuf.
//
//   * `send_dmabuf` (zero copy): one dma-buf that the compositor has just
//     rendered into (screencopy with linux-dmabuf buffers, as wf-recorder
//     captures).  The buffer is wrapped in an AVDRMFrameDescriptor,
//     mapped to a VAAPI surface with av_hwframe_map — the GPU imports
//     the buffer, no pixels pass through the CPU — and a filtergraph
//     (`scale_vaapi=format=nv12`) converts the packed RGB to the NV12
//     the encoder wants, again on the GPU.  This is the path that makes
//     4K at 60+ fps possible: the measured software path spends ~40 ms
//     per 4K frame in the compositor's shm readback and CPU colour
//     conversion, which caps it near 21 fps.
//
// The library is loaded with dlopen at runtime: vshot's screenshots must
// keep working on a machine without ffmpeg libraries, so the dependency
// stays optional and a missing library is reported by `record` alone.
// The functions are resolved once into a table; nothing here links against
// libav* at build time except its headers.  libavformat and libavfilter are
// loaded as optional extras: the MP4 muxer and the zero-copy path each
// report their own absence.
//
// The interface Rust sees is one recorder:
//   vshot_rec_start(path, width, height, codec, qp)   -> handle or NULL
//   vshot_rec_start_dmabuf(path, width, height, codec, qp, fourcc)
//   vshot_rec_frame(handle, rgba, duration_ms)
//   vshot_rec_frame_dmabuf(handle, fd, fourcc, modifier, offset, stride, duration_ms)
//   vshot_rec_resize_fit(handle, width, height)       -> follow a source resize
//   vshot_rec_start_mic(path, width, height, codec, qp, rate, channels)
//   vshot_rec_start_dmabuf_mic(path, width, height, codec, qp, fourcc, rate, channels)
//   vshot_rec_audio_feed(handle, float samples, frames)
//   vshot_rec_audio_pump(handle) / vshot_rec_audio_rate(handle)
//   vshot_rec_audio_channels(handle) / vshot_rec_audio_error(handle)
//   vshot_rec_finish(handle)                          -> trailer written
//   vshot_rec_frames(handle) / vshot_rec_seconds(handle)
//   vshot_rec_free(handle)
//   vshot_rec_last_error(handle)                      -> a message
//   vshot_rec_available() / vshot_rec_load_error()
//
// Under it sits the encoder alone (`vshot_av_enc_*`), which turns frames
// into packets; the recorder hands those packets straight to libavformat's
// MP4 muxer, so the container is written by the library that produced the
// bitstream rather than reconstructed from one.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>

// The GBM types are declared opaque here rather than through <gbm.h>: the
// shim only ever passes pointers through, and skipping the header keeps a
// build machine without the GBM development headers able to compile it.
struct gbm_device;
struct gbm_bo;

// ---------------------------------------------------------------------------
// GBM: the dma-buf allocator for the zero-copy path
// ---------------------------------------------------------------------------
//
// The compositor renders a screencopy frame into a wl_buffer the client
// provides; for a zero-copy recording that buffer has to be a dma-buf the
// VAAPI encoder can import.  GBM allocates such buffers on the same DRM
// device; this section loads libgbm with dlopen (it is a separate optional
// library, like libavfilter) and wraps the few calls the buffer pool needs.
// The BO's file descriptor is held here and closed when the buffer dies;
// the caller passes it around but never owns it.

// The dlopen helpers come from the libav* section below; forward them so
// GBM can load first.
static void *try_open(const char *const *names);
static void *sym(void *handle, const char *name);

typedef struct VshotGbmApi {
    struct gbm_device *(*create_device)(int fd);
    void (*device_destroy)(struct gbm_device *dev);
    struct gbm_bo *(*bo_create_with_modifiers)(struct gbm_device *dev, uint32_t width,
                                               uint32_t height, uint32_t format,
                                               const uint64_t *modifiers, const unsigned int count);
    struct gbm_bo *(*bo_create)(struct gbm_device *dev, uint32_t width, uint32_t height,
                                uint32_t format, uint32_t flags);
    int (*bo_get_fd)(struct gbm_bo *bo);
    uint32_t (*bo_get_stride)(struct gbm_bo *bo);
    uint32_t (*bo_get_offset)(struct gbm_bo *bo, int plane);
    uint64_t (*bo_get_modifier)(struct gbm_bo *bo);
    uint32_t (*bo_get_format)(struct gbm_bo *bo);
    int (*bo_get_plane_count)(struct gbm_bo *bo);
    uint32_t (*bo_get_width)(struct gbm_bo *bo);
    uint32_t (*bo_get_height)(struct gbm_bo *bo);
    void (*bo_destroy)(struct gbm_bo *bo);
} VshotGbmApi;

static VshotGbmApi *gbm_api = NULL;
static char gbm_error[256];
static struct gbm_device *gbm_device = NULL;

static VshotGbmApi *load_gbm(void) {
    static int state = 0;
    static VshotGbmApi table;
    if (state != 0) {
        return state > 0 ? &table : NULL;
    }
    static const char *const names[] = {"libgbm.so.1", "libgbm.so", NULL};
    void *handle = try_open(names);
    if (!handle) {
        snprintf(gbm_error, sizeof(gbm_error),
                 "libgbm is not installed, so zero-copy recording is unavailable "
                 "(it comes with mesa)");
        state = -1;
        return NULL;
    }
#define GNEED(dst, name)                                                       \
    do {                                                                       \
        (dst) = (void *)(uintptr_t)sym(handle, (name));                        \
        if (!(dst)) {                                                          \
            snprintf(gbm_error, sizeof(gbm_error), "libgbm is missing %s",     \
                     (name));                                                  \
            state = -1;                                                       \
            return NULL;                                                      \
        }                                                                      \
    } while (0)
    GNEED(table.create_device, "gbm_create_device");
    GNEED(table.device_destroy, "gbm_device_destroy");
    GNEED(table.bo_create_with_modifiers, "gbm_bo_create_with_modifiers");
    GNEED(table.bo_create, "gbm_bo_create");
    GNEED(table.bo_get_fd, "gbm_bo_get_fd");
    GNEED(table.bo_get_stride, "gbm_bo_get_stride");
    GNEED(table.bo_get_offset, "gbm_bo_get_offset");
    GNEED(table.bo_get_modifier, "gbm_bo_get_modifier");
    GNEED(table.bo_get_format, "gbm_bo_get_format");
    GNEED(table.bo_get_plane_count, "gbm_bo_get_plane_count");
    GNEED(table.bo_get_width, "gbm_bo_get_width");
    GNEED(table.bo_get_height, "gbm_bo_get_height");
    GNEED(table.bo_destroy, "gbm_bo_destroy");
#undef GNEED
    state = 1;
    return &table;
}

// The one GBM device the process uses, created on first demand from the
// render node the encoder uses too.
static struct gbm_device *ensure_gbm_device(void) {
    if (gbm_device) {
        return gbm_device;
    }
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        snprintf(gbm_error, sizeof(gbm_error), "could not open /dev/dri/renderD128: %s",
                 strerror(errno));
        return NULL;
    }
    gbm_device = gbm_api->create_device(fd);
    if (!gbm_device) {
        close(fd);
        snprintf(gbm_error, sizeof(gbm_error), "gbm_create_device failed on /dev/dri/renderD128");
        return NULL;
    }
    return gbm_device;
}

// One allocated buffer: the BO plus the descriptor facts the caller needs.
// The fd is owned here (closed by destroy); the caller uses it but must not
// close it.
typedef struct VshotGbmBuffer {
    struct gbm_bo *bo;
    int fd;
    uint64_t modifier;
    uint32_t fourcc;
    uint32_t stride;
    uint32_t offset;
    int width;
    int height;
} VshotGbmBuffer;

// Wraps a freshly allocated BO in the handle the caller gets: the fd is
// exported here and owned by the handle, and the descriptor facts are read
// back from the driver.  On failure the BO is destroyed and NULL returned.
static void *wrap_gbm_bo(struct gbm_bo *bo, int width, int height, uint64_t *modifier_out,
                         int *fd_out, unsigned *stride_out, unsigned *offset_out) {
    int fd = gbm_api->bo_get_fd(bo);
    if (fd < 0) {
        gbm_api->bo_destroy(bo);
        snprintf(gbm_error, sizeof(gbm_error), "could not export the dma-buf file descriptor");
        return NULL;
    }
    VshotGbmBuffer *buffer = calloc(1, sizeof(VshotGbmBuffer));
    if (!buffer) {
        close(fd);
        gbm_api->bo_destroy(bo);
        return NULL;
    }
    buffer->bo = bo;
    buffer->fd = fd;
    buffer->modifier = gbm_api->bo_get_modifier(bo);
    buffer->fourcc = gbm_api->bo_get_format(bo);
    buffer->stride = gbm_api->bo_get_stride(bo);
    buffer->offset = gbm_api->bo_get_offset(bo, 0);
    buffer->width = width;
    buffer->height = height;
    if (modifier_out) {
        *modifier_out = buffer->modifier;
    }
    if (fd_out) {
        *fd_out = buffer->fd;
    }
    if (stride_out) {
        *stride_out = buffer->stride;
    }
    if (offset_out) {
        *offset_out = buffer->offset;
    }
    return buffer;
}

// Allocates one dma-buf of the requested format and size, preferring a
// linear layout (what screencopy buffers use and what the VAAPI import
// expects), falling back to a plain renderable allocation.  Outputs the
// descriptor facts on success.
void *vshot_gbm_buffer_create(int width, int height, unsigned fourcc, uint64_t *modifier_out,
                              int *fd_out, unsigned *stride_out, unsigned *offset_out) {
    gbm_api = load_gbm();
    if (!gbm_api) {
        return NULL;
    }
    struct gbm_device *device = ensure_gbm_device();
    if (!device) {
        return NULL;
    }
    if (width <= 0 || height <= 0) {
        snprintf(gbm_error, sizeof(gbm_error), "invalid buffer size %dx%d", width, height);
        return NULL;
    }
    struct gbm_bo *bo = NULL;
    if (gbm_api->bo_create_with_modifiers) {
        const uint64_t linear = 0; // DRM_FORMAT_MOD_LINEAR
        bo = gbm_api->bo_create_with_modifiers(device, (uint32_t)width, (uint32_t)height, fourcc,
                                               &linear, 1);
    }
    if (!bo) {
        // GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING = 0x2 | 0x4
        bo = gbm_api->bo_create(device, (uint32_t)width, (uint32_t)height, fourcc, 0x2 | 0x4);
    }
    if (!bo) {
        snprintf(gbm_error, sizeof(gbm_error),
                 "could not allocate a %dx%d dma-buf (fourcc 0x%08x)", width, height, fourcc);
        return NULL;
    }
    return wrap_gbm_bo(bo, width, height, modifier_out, fd_out, stride_out, offset_out);
}

// Allocates one dma-buf from the modifiers the compositor advertised, which
// is what an `ext_image_copy_capture` client has to do: there the client is
// the side that allocates the buffer a window gets copied into, so the
// modifier has to be one the compositor can import.  GBM picks the first
// modifier of the list it can allocate for the format.
void *vshot_gbm_buffer_create_with_modifiers(int width, int height, unsigned fourcc,
                                             const uint64_t *modifiers, int count,
                                             uint64_t *modifier_out, int *fd_out,
                                             unsigned *stride_out, unsigned *offset_out) {
    gbm_api = load_gbm();
    if (!gbm_api) {
        return NULL;
    }
    struct gbm_device *device = ensure_gbm_device();
    if (!device) {
        return NULL;
    }
    if (width <= 0 || height <= 0) {
        snprintf(gbm_error, sizeof(gbm_error), "invalid buffer size %dx%d", width, height);
        return NULL;
    }
    if (!modifiers || count <= 0) {
        snprintf(gbm_error, sizeof(gbm_error),
                 "the compositor offered no dma-buf modifier for a %dx%d buffer", width, height);
        return NULL;
    }
    if (!gbm_api->bo_create_with_modifiers) {
        snprintf(gbm_error, sizeof(gbm_error),
                 "this libgbm cannot allocate by modifier, which a window capture needs");
        return NULL;
    }
    struct gbm_bo *bo = gbm_api->bo_create_with_modifiers(
        device, (uint32_t)width, (uint32_t)height, fourcc, modifiers, count);
    if (!bo) {
        snprintf(gbm_error, sizeof(gbm_error),
                 "could not allocate a %dx%d dma-buf (fourcc 0x%08x) with any of the %d modifiers "
                 "the compositor offered",
                 width, height, fourcc, count);
        return NULL;
    }
    return wrap_gbm_bo(bo, width, height, modifier_out, fd_out, stride_out, offset_out);
}

int vshot_gbm_buffer_fd(void *handle) {
    VshotGbmBuffer *buffer = handle;
    return buffer ? buffer->fd : -1;
}

void vshot_gbm_buffer_destroy(void *handle) {
    VshotGbmBuffer *buffer = handle;
    if (!buffer) {
        return;
    }
    // The mapping cache in the encoder session may still hold a VAAPI
    // surface imported from this fd; the caller must destroy the encoder
    // first (record.rs does), so closing here is safe.
    if (buffer->fd >= 0) {
        close(buffer->fd);
    }
    if (buffer->bo && gbm_api) {
        gbm_api->bo_destroy(buffer->bo);
    }
    free(buffer);
}

int vshot_gbm_available(void) { return load_gbm() != NULL; }

const char *vshot_gbm_load_error(void) { return gbm_error; }


// Debug tracing: with VSHOT_RECORD_DEBUG in the environment, each send
// reports its internal stages (convert/upload/filter/send/drain, in
// milliseconds on the monotonic clock), so a slow frame size can be
// attributed to the right stage without a profiler.  Checked once per
// process.
static int trace_enabled(void) {
    static int value = -1;
    if (value < 0) {
        value = getenv("VSHOT_RECORD_DEBUG") != NULL;
    }
    return value;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

// ---------------------------------------------------------------------------
// The runtime table
// ---------------------------------------------------------------------------

typedef struct VshotAvApi {
    // libavcodec
    const AVCodec *(*find_encoder_by_name)(const char *name);
    AVCodecContext *(*alloc_context3)(const AVCodec *codec);
    void (*free_context)(AVCodecContext **ctx);
    int (*open2)(AVCodecContext *ctx, const AVCodec *codec, AVDictionary **options);
    int (*send_frame)(AVCodecContext *ctx, const AVFrame *frame);
    int (*receive_packet)(AVCodecContext *ctx, AVPacket *pkt);
    AVPacket *(*packet_alloc)(void);
    void (*packet_free)(AVPacket **pkt);
    void (*packet_unref)(AVPacket *pkt);
    int (*packet_ref)(AVPacket *dst, const AVPacket *src);
    unsigned (*codec_version)(void);
    // libavutil
    AVFrame *(*frame_alloc)(void);
    void (*frame_free)(AVFrame **frame);
    void (*frame_unref)(AVFrame *frame);
    int (*frame_ref)(AVFrame *dst, const AVFrame *src);
    int (*hwdevice_ctx_create)(AVBufferRef **device_ctx, enum AVHWDeviceType type,
                               const char *device, AVDictionary *opts, int flags);
    int (*hwdevice_ctx_init)(AVBufferRef *ref);
    AVBufferRef *(*hwframe_ctx_alloc)(AVBufferRef *device_ctx);
    int (*hwframe_ctx_init)(AVBufferRef *ref);
    int (*hwframe_get_buffer)(AVBufferRef *hwframe_ctx, AVFrame *frame, int flags);
    int (*hwframe_transfer_data)(AVFrame *dst, const AVFrame *src, int flags);
    int (*hwframe_map)(AVFrame *dst, const AVFrame *src, int flags);
    AVBufferRef *(*buffer_ref)(AVBufferRef *buf);
    void (*buffer_unref)(AVBufferRef **buf);
    AVBufferRef *(*buffer_create)(uint8_t *data, size_t size,
                                  void (*free)(void *opaque, uint8_t *data),
                                  void *opaque, int flags);
    char *(*strdup)(const char *s);
    int (*opt_set)(void *obj, const char *name, const char *val, int search_flags);
    int (*opt_set_int)(void *obj, const char *name, int64_t val, int search_flags);
    int (*strerror)(int errnum, char *errbuf, size_t errbuf_size);
    int64_t (*rescale_q)(int64_t a, AVRational bq, AVRational cq);
    void (*log_set_level)(int level);
    void (*buffer_default_free)(void *opaque, uint8_t *data);
    // The audio side (`record --mic`): one more frame allocator and the
    // channel-layout constructor.  The latter is optional — a build of
    // ffmpeg old enough to lack `av_channel_layout_default` still records
    // audio, with the layout left to the encoder's default for the count.
    int (*frame_get_buffer)(AVFrame *frame, int align);
    void (*channel_layout_default)(AVChannelLayout *layout, int channels);
    // libavformat (optional; the recorder's MP4 muxer is the only user)
    int format_loaded;
    int (*format_alloc_output)(AVFormatContext **ctx, const AVOutputFormat *oformat,
                               const char *format_name, const char *filename);
    AVStream *(*format_new_stream)(AVFormatContext *ctx, const AVCodec *codec);
    int (*format_write_header)(AVFormatContext *ctx, AVDictionary **options);
    int (*format_write_frame)(AVFormatContext *ctx, AVPacket *pkt);
    int (*format_write_trailer)(AVFormatContext *ctx);
    void (*format_free_context)(AVFormatContext *ctx);
    int (*io_open)(AVIOContext **s, const char *url, int flags);
    void (*io_closep)(AVIOContext **s);
    int (*parameters_from_context)(AVCodecParameters *par, const AVCodecContext *codec);
    // libavfilter (optional; the zero-copy path needs these)
    int filter_loaded;
    const AVFilter *(*filter_get_by_name)(const char *name);
    AVFilterGraph *(*filter_graph_alloc)(void);
    AVFilterContext *(*filter_graph_alloc_filter)(AVFilterGraph *graph,
                                                  const AVFilter *filter,
                                                  const char *name);
    void (*filter_graph_free)(AVFilterGraph **graph);
    int (*filter_init_str)(AVFilterContext *ctx, const char *args);
    int (*filter_init_dict)(AVFilterContext *ctx, AVDictionary **options);
    int (*filter_graph_parse_ptr)(AVFilterGraph *graph, const char *filters,
                                  AVFilterInOut **inputs, AVFilterInOut **outputs,
                                  void *log_ctx);
    int (*filter_graph_config)(AVFilterGraph *graphctx, void *log_ctx);
    AVFilterInOut *(*filter_inout_alloc)(void);
    void (*filter_inout_free)(AVFilterInOut **inout);
    AVBufferSrcParameters *(*buffersrc_parameters_alloc)(void);
    int (*buffersrc_parameters_set)(AVFilterContext *ctx, AVBufferSrcParameters *par);
    int (*buffersrc_add_frame_flags)(AVFilterContext *buffer_src, AVFrame *frame, int flags);
    int (*buffersink_get_frame)(AVFilterContext *ctx, AVFrame *frame);
    AVBufferRef *(*buffersink_get_hw_frames_ctx)(AVFilterContext *ctx);
} VshotAvApi;

static VshotAvApi *api = NULL;
static char api_error[256];
// The create path destroys its half-built encoder on failure; this keeps
// the reason alive for `vshot_av_enc_load_error` to report.
static char create_error[512];

// Forward declarations: `vshot_av_enc_create` calls `destroy` on a partial
// open, and the exported functions are defined after the helpers.
typedef struct VshotAvEnc VshotAvEnc;
void vshot_av_enc_destroy(VshotAvEnc *enc);
int vshot_av_enc_send(VshotAvEnc *enc, const uint8_t *rgba);
int vshot_av_enc_send_dmabuf(VshotAvEnc *enc, int fd, unsigned fourcc, uint64_t modifier,
                             int offset, int stride);
int vshot_av_enc_resize_fit(VshotAvEnc *enc, int in_w, int in_h, unsigned fourcc);
int vshot_av_enc_flush(VshotAvEnc *enc);
const uint8_t *vshot_av_enc_take(VshotAvEnc *enc, int *len);
const uint8_t *vshot_av_enc_extradata(VshotAvEnc *enc, int *len);
const char *vshot_av_enc_last_error(VshotAvEnc *enc);
int vshot_av_enc_available(void);
int vshot_av_enc_filter_available(void);
const char *vshot_av_enc_load_error(void);
unsigned vshot_av_enc_version(void);
// A one-shot probe of a hardware backend: can it be opened at all on this
// machine?  Used by `--encoder-backend auto` to pick one and by the Rust side
// to fall back to the software path when the GPU's encoder cannot import a
// compositor buffer.  `backend` is VSHOT_BACKEND_VAAPI or VSHOT_BACKEND_NVENC.
int vshot_av_enc_backend_probe(int backend, char *err, size_t err_len);

static void *try_open(const char *const *names) {
    for (int i = 0; names[i]; i++) {
        void *handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            return handle;
        }
    }
    return NULL;
}

static void *sym(void *handle, const char *name) {
    return dlsym(handle, name);
}

// Loads libavcodec + libavutil (mandatory) and libavfilter (optional) once.
// Returns NULL and fills `api_error` on hard failure.  Successive versions
// are tried so a rolling-release system that bumps the soname keeps working
// without a rebuild.
static VshotAvApi *load_api(void) {
    static int state = 0; // 0 = untouched, 1 = loaded, -1 = failed
    static VshotAvApi table;
    if (state != 0) {
        return state > 0 ? &table : NULL;
    }
    static const char *const codec_names[] = {
        "libavcodec.so.63", "libavcodec.so.62", "libavcodec.so.61",
        "libavcodec.so.60", "libavcodec.so", NULL,
    };
    static const char *const util_names[] = {
        "libavutil.so.61", "libavutil.so.60", "libavutil.so.59",
        "libavutil.so.58", "libavutil.so", NULL,
    };
    static const char *const format_names[] = {
        "libavformat.so.63", "libavformat.so.62", "libavformat.so.61",
        "libavformat.so.60", "libavformat.so.59", "libavformat.so", NULL,
    };
    static const char *const filter_names[] = {
        "libavfilter.so.12", "libavfilter.so.11", "libavfilter.so.10",
        "libavfilter.so.9", "libavfilter.so.8", "libavfilter.so", NULL,
    };
    void *codec = try_open(codec_names);
    void *util = try_open(util_names);
    if (!codec || !util) {
        snprintf(api_error, sizeof(api_error),
                 "libavcodec/libavutil are not installed, so GPU recording is "
                 "unavailable (install ffmpeg)");
        state = -1;
        return NULL;
    }
#define NEED(dst, handle, name)                                                \
    do {                                                                       \
        (dst) = (void *)(uintptr_t)sym((handle), (name));                      \
        if (!(dst)) {                                                          \
            snprintf(api_error, sizeof(api_error),                            \
                     "libav* is missing the symbol %s", (name));               \
            state = -1;                                                       \
            return NULL;                                                      \
        }                                                                      \
    } while (0)
#define OPT(dst, handle, name)                                                 \
    do {                                                                       \
        (dst) = (void *)(uintptr_t)sym((handle), (name));                      \
    } while (0)
    NEED(table.find_encoder_by_name, codec, "avcodec_find_encoder_by_name");
    NEED(table.alloc_context3, codec, "avcodec_alloc_context3");
    NEED(table.free_context, codec, "avcodec_free_context");
    NEED(table.open2, codec, "avcodec_open2");
    NEED(table.send_frame, codec, "avcodec_send_frame");
    NEED(table.receive_packet, codec, "avcodec_receive_packet");
    NEED(table.packet_alloc, codec, "av_packet_alloc");
    NEED(table.packet_free, codec, "av_packet_free");
    NEED(table.packet_unref, codec, "av_packet_unref");
    NEED(table.packet_ref, codec, "av_packet_ref");
    NEED(table.codec_version, codec, "avcodec_version");
    NEED(table.frame_alloc, util, "av_frame_alloc");
    NEED(table.frame_free, util, "av_frame_free");
    NEED(table.frame_unref, util, "av_frame_unref");
    NEED(table.frame_ref, util, "av_frame_ref");
    NEED(table.hwdevice_ctx_create, util, "av_hwdevice_ctx_create");
    NEED(table.hwdevice_ctx_init, util, "av_hwdevice_ctx_init");
    NEED(table.hwframe_ctx_alloc, util, "av_hwframe_ctx_alloc");
    NEED(table.hwframe_ctx_init, util, "av_hwframe_ctx_init");
    NEED(table.hwframe_get_buffer, util, "av_hwframe_get_buffer");
    NEED(table.hwframe_transfer_data, util, "av_hwframe_transfer_data");
    NEED(table.hwframe_map, util, "av_hwframe_map");
    NEED(table.buffer_ref, util, "av_buffer_ref");
    NEED(table.buffer_unref, util, "av_buffer_unref");
    NEED(table.buffer_create, util, "av_buffer_create");
    NEED(table.strdup, util, "av_strdup");
    NEED(table.opt_set, util, "av_opt_set");
    NEED(table.opt_set_int, util, "av_opt_set_int");
    NEED(table.strerror, util, "av_strerror");
    NEED(table.rescale_q, util, "av_rescale_q");
    NEED(table.log_set_level, util, "av_log_set_level");
    NEED(table.frame_get_buffer, util, "av_frame_get_buffer");
    // Optional: a pre-5.0 ffmpeg has no channel-layout API at all, and its
    // AAC encoder reads the channel count from the context instead.
    OPT(table.channel_layout_default, util, "av_channel_layout_default");
#undef NEED
    // libavformat is optional: only the recorder's MP4 muxer uses it, and
    // its absence disables `record` while leaving screenshots alone.
    // `avcodec_parameters_from_context` (libavcodec) is the call that hands
    // the stream its codec parameters, exactly as wf-recorder does it.
    OPT(table.parameters_from_context, codec, "avcodec_parameters_from_context");
    void *format = try_open(format_names);
    if (format) {
        OPT(table.format_alloc_output, format, "avformat_alloc_output_context2");
        OPT(table.format_new_stream, format, "avformat_new_stream");
        OPT(table.format_write_header, format, "avformat_write_header");
        OPT(table.format_write_frame, format, "av_write_frame");
        OPT(table.format_write_trailer, format, "av_write_trailer");
        OPT(table.format_free_context, format, "avformat_free_context");
        OPT(table.io_open, format, "avio_open");
        OPT(table.io_closep, format, "avio_closep");
        table.format_loaded = table.parameters_from_context && table.format_alloc_output &&
                              table.format_new_stream && table.format_write_header &&
                              table.format_write_frame && table.format_write_trailer &&
                              table.format_free_context && table.io_open && table.io_closep;
    }
    // libavfilter is optional: only the zero-copy path uses it, and its
    // absence must not break the software path.
    void *filter = try_open(filter_names);
    if (filter) {
        OPT(table.filter_get_by_name, filter, "avfilter_get_by_name");
        OPT(table.filter_graph_alloc, filter, "avfilter_graph_alloc");
        OPT(table.filter_graph_alloc_filter, filter, "avfilter_graph_alloc_filter");
        OPT(table.filter_graph_free, filter, "avfilter_graph_free");
        OPT(table.filter_init_str, filter, "avfilter_init_str");
        OPT(table.filter_init_dict, filter, "avfilter_init_dict");
        OPT(table.filter_graph_parse_ptr, filter, "avfilter_graph_parse_ptr");
        OPT(table.filter_graph_config, filter, "avfilter_graph_config");
        OPT(table.filter_inout_alloc, filter, "avfilter_inout_alloc");
        OPT(table.filter_inout_free, filter, "avfilter_inout_free");
        OPT(table.buffersrc_parameters_alloc, filter, "av_buffersrc_parameters_alloc");
        OPT(table.buffersrc_parameters_set, filter, "av_buffersrc_parameters_set");
        OPT(table.buffersrc_add_frame_flags, filter, "av_buffersrc_add_frame_flags");
        OPT(table.buffersink_get_frame, filter, "av_buffersink_get_frame");
        OPT(table.buffersink_get_hw_frames_ctx, filter, "av_buffersink_get_hw_frames_ctx");
        table.filter_loaded = table.filter_get_by_name && table.filter_graph_alloc &&
                              table.filter_graph_alloc_filter && table.filter_graph_free &&
                              table.filter_init_str && table.filter_init_dict &&
                              table.filter_graph_parse_ptr && table.filter_graph_config &&
                              table.filter_inout_alloc && table.filter_inout_free &&
                              table.buffersrc_parameters_alloc &&
                              table.buffersrc_parameters_set &&
                              table.buffersrc_add_frame_flags && table.buffersink_get_frame &&
                              table.buffersink_get_hw_frames_ctx;
    }
#undef OPT
    state = 1;
    return &table;
}

// ---------------------------------------------------------------------------
// The encoder
// ---------------------------------------------------------------------------

// How many dma-buf buffers one session can hold mapped at once.  The
// recorder cycles through its buffer pool with this many entries; a larger
// pool here would only keep stale mappings alive.
#define VSHOT_MAX_MAPS 64

typedef struct VshotDmabufMap {
    int fd;         // the dma-buf this mapping belongs to (key)
    AVFrame *frame; // a VAAPI frame mapped from it (READ)
} VshotDmabufMap;

// Durations waiting for their packets, for the recorder's timeline.  A VAAPI
// encoder keeps one or two frames in flight; anything beyond this many means
// something is deeply wrong, and the queue drops its oldest entry rather than
// writing out of bounds.
#define VSHOT_MAX_PENDING 64

// ---------------------------------------------------------------------------
// The replay ring: encoded packets kept in memory, newest window only
// ---------------------------------------------------------------------------
// A replay session encodes exactly like a recording, but its packets go into
// this ring instead of a file.  Nothing reaches the disk until a save is
// triggered, and then the packets are copied straight into an MP4 (a stream
// copy, no re-encode), so the steady state costs one encode and one in-memory
// push — a recording minus the disk write — and the trigger costs one mux.
//
// Eviction is by time: the ring keeps `window_ms` of history, which the caller
// sizes as the user's window plus one key-frame interval — a save starts at the
// newest key frame that is not newer than `newest - window`, and a key frame
// sits up to one GOP before that edge.  An MP4 whose first video packet is not
// a key frame shows nothing, which is why the start is key-frame aligned rather
// than cut at the exact window edge.

typedef struct VshotReplayPkt {
    AVPacket *pkt;  // our own reference to the encoder's packet
    int is_audio;
    int is_key;
    int64_t ms;     // presentation time on the millisecond timeline
    int64_t dur_ms; // how long the packet covers
} VshotReplayPkt;

typedef struct VshotReplay {
    VshotReplayPkt *pkts;
    int cap;
    int head; // index of the oldest entry
    int count;
    int64_t window_ms;
    int64_t newest_ms; // end of the newest packet
    int64_t oldest_ms; // start of the oldest packet
    int64_t saved;     // how many saves have run
} VshotReplay;

static VshotReplay *replay_create(int64_t window_ms, int fps) {
    VshotReplay *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    // Room for the window at the rate the session actually runs, plus its
    // audio: a video packet per frame and roughly one AAC packet per 21 ms.
    // Sizing on the real rate (not the CLI ceiling) keeps a 4K120 ring a
    // quarter of what a 240-fps worst case would reserve, while still holding
    // the whole window.  The extra second is the slack a save scans back
    // through for a key frame.
    if (fps < 1) {
        fps = 1;
    }
    if (fps > 480) {
        fps = 480;
    }
    int64_t seconds = (window_ms + 1000) / 1000 + 1;
    int64_t per_sec = (int64_t)fps + 48; // video frames + ~48 audio packets a second
    int64_t cap = seconds * per_sec + 1024;
    if (cap > 1048576) {
        cap = 1048576;
    }
    r->pkts = calloc((size_t)cap, sizeof(*r->pkts));
    if (!r->pkts) {
        free(r);
        return NULL;
    }
    r->cap = (int)cap;
    r->window_ms = window_ms;
    return r;
}

static void replay_evict_one(VshotReplay *r) {
    if (r->count == 0) {
        return;
    }
    VshotReplayPkt *e = &r->pkts[r->head];
    if (e->pkt) {
        api->packet_unref(e->pkt);
        api->packet_free(&e->pkt);
    }
    r->head = (r->head + 1) % r->cap;
    r->count--;
    r->oldest_ms = r->count > 0 ? r->pkts[r->head].ms : r->newest_ms;
}

static void replay_destroy(VshotReplay *r) {
    if (!r) {
        return;
    }
    while (r->count > 0) {
        replay_evict_one(r);
    }
    free(r->pkts);
    free(r);
}

// Keeps one packet's worth of history.  The packet is reference-copied, so the
// caller is free to unref its own as soon as this returns.
static int replay_push(VshotReplay *r, AVPacket *src, int is_audio, int is_key, int64_t ms,
                       int64_t dur_ms) {
    if (!r) {
        return 0;
    }
    if (r->count == r->cap) {
        replay_evict_one(r);
    }
    int slot = (r->head + r->count) % r->cap;
    AVPacket *copy = api->packet_alloc();
    if (!copy) {
        return -1;
    }
    if (api->packet_ref(copy, src) < 0) {
        api->packet_free(&copy);
        return -1;
    }
    r->pkts[slot].pkt = copy;
    r->pkts[slot].is_audio = is_audio;
    r->pkts[slot].is_key = is_key;
    r->pkts[slot].ms = ms;
    r->pkts[slot].dur_ms = dur_ms > 0 ? dur_ms : 1;
    r->count++;
    if (ms + r->pkts[slot].dur_ms > r->newest_ms) {
        r->newest_ms = ms + r->pkts[slot].dur_ms;
    }
    if (r->count == 1) {
        r->oldest_ms = ms;
    }
    // Time-based eviction.  The caller sized the ring's window as the user's
    // window plus one key-frame interval, so evicting past it would drop the
    // key frame a save of the full window needs.
    int64_t retain = r->window_ms;
    while (r->count > 1 && r->oldest_ms < r->newest_ms - retain) {
        replay_evict_one(r);
    }
    return 0;
}

// The two hardware encoder backends.  VAAPI is the default on AMD and Intel
// (a render node), NVENC the NVIDIA one (a CUDA device).  They differ in the
// hw device type, the frames' pixel format, the encoder name suffix and the
// private option names — every one of those reads `enc->backend` rather than
// assuming VAAPI, so a session is one backend end to end.
#define VSHOT_BACKEND_VAAPI 0
#define VSHOT_BACKEND_NVENC 1

struct VshotAvEnc {
    AVBufferRef *device;
    AVBufferRef *frames;    // NV12 pool for the software path
    AVCodecContext *ctx;
    // Which hardware encoder this session runs: VSHOT_BACKEND_VAAPI or
    // VSHOT_BACKEND_NVENC.  Chosen once at open time from `--encoder-backend`.
    int backend;
    // The software path's letterbox staging: a canvas-sized RGBA buffer the
    // incoming frame is fitted into when the source was resized mid-session.
    // A dma-buf session does that fitting in its filtergraph; this one has no
    // graph, so it is done on the CPU here.  Allocated lazily, on the first
    // frame that needs it.
    uint8_t *sw_fit;        // canvas-sized RGBA scratch
    size_t sw_fit_cap;
    AVFrame *sw;            // reused NV12 software frame (software path)
    // --- replay ---
    // When set, the packets go into this ring instead of a muxer or the raw
    // byte buffer: a recording that keeps its last window in memory.
    VshotReplay *replay;
    int64_t replay_next_ms; // the video timeline, in milliseconds
    // --- zero-copy path ---
    int dmabuf;             // 1 when this session encodes dma-bufs
    unsigned fourcc;        // the dma-buf format this session was opened for
    // The size of the dma-buf frames arriving from the capture side.  It is
    // the encoder's own size until the source is resized mid-recording
    // (`vshot_rec_resize_fit`), after which the filtergraph fits these
    // frames into the encoder's canvas.
    int in_w;
    int in_h;
    AVFilterGraph *graph;
    AVFilterContext *graph_src;
    AVFilterContext *graph_sink;
    // The fit chain's second input: the canvas-sized black plate the letterbox
    // is composed over (see `open_filtergraph_dmabuf_fit`).  Only a fit graph
    // has one; a graph built for the encoder's own size runs the two-filter
    // chain and leaves these NULL.
    AVFilterContext *graph_plate; // the plate's buffer source
    AVBufferRef *plate_frames;    // BGRA pool of canvas-sized plates
    AVFrame *plate;               // the one black plate fed with every frame
    VshotDmabufMap maps[VSHOT_MAX_MAPS];
    int map_count;
    // --- output ---
    uint8_t *out;        // accumulated annex-b bytes (this send's packets)
    size_t out_len;
    size_t out_cap;
    // When a muxer is attached (the recorder's mode) packets go straight
    // into it instead of accumulating in `out`: libavformat needs the
    // packets themselves, so that it can carry the codec's own parameter
    // sets and key-frame flags into the container rather than us
    // reconstructing them from a byte stream.
    AVFormatContext *mux;
    AVStream *mux_stream;
    AVRational mux_time_base; // the muxer's, read back after the header
    int64_t mux_next_pts;     // milliseconds, our own timeline
    // The durations of the frames sent but not yet handed back as packets.
    // A hardware encoder returns a packet a frame or two late, so the
    // duration that belongs to a packet arrives *before* the packet does;
    // pairing them through a queue is what keeps the timeline exact whatever
    // the encoder's internal depth turns out to be.  A single "current
    // duration" shifts the whole timeline by that delay (and the first frame
    // is the one that pays for it).
    int pending_ms[VSHOT_MAX_PENDING];
    int pending_head;
    int pending_count;
    int64_t mux_frames;
    int frames_sent;
    int width;
    int height;
    char err[256];
};

// Durations waiting for their packets: sent on the frame's way in, taken on
// the packet's way out.
static void push_duration(VshotAvEnc *enc, int ms) {
    if (enc->pending_count == VSHOT_MAX_PENDING) {
        enc->pending_head = (enc->pending_head + 1) % VSHOT_MAX_PENDING;
        enc->pending_count--;
    }
    int tail = (enc->pending_head + enc->pending_count) % VSHOT_MAX_PENDING;
    enc->pending_ms[tail] = ms > 0 ? ms : 1;
    enc->pending_count++;
}

static int pop_duration(VshotAvEnc *enc) {
    if (enc->pending_count == 0) {
        // A packet no frame asked for: give it a millisecond rather than
        // letting the timeline stop.
        return 1;
    }
    int ms = enc->pending_ms[enc->pending_head];
    enc->pending_head = (enc->pending_head + 1) % VSHOT_MAX_PENDING;
    enc->pending_count--;
    return ms;
}

static void set_err(VshotAvEnc *enc, const char *what, int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (api && api->strerror) {
        api->strerror(code, text, sizeof(text));
    }
    snprintf(enc->err, sizeof(enc->err), "%s: %s (%d)", what,
             text[0] ? text : "unknown error", code);
}

// Delivers every packet the encoder currently has ready.
static int drain_packets(VshotAvEnc *enc) {
    AVPacket *pkt = api->packet_alloc();
    if (!pkt) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate a packet");
        return -1;
    }
    for (;;) {
        int ret = api->receive_packet(enc->ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            set_err(enc, "receiving an encoded packet failed", ret);
            api->packet_free(&pkt);
            return -1;
        }
        if (pkt->size > 0 && enc->replay) {
            // The replay ring: keep the packet (a reference copy) and its
            // place on the millisecond timeline, and drop the oldest history
            // past the window.  Nothing is written anywhere — that is the
            // whole point of a replay.
            int64_t duration = pop_duration(enc);
            int is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            int64_t ms = enc->replay_next_ms;
            enc->replay_next_ms += duration;
            if (replay_push(enc->replay, pkt, 0, is_key, ms, duration) != 0) {
                set_err(enc, "keeping a packet in the replay ring failed", 0);
                api->packet_free(&pkt);
                return -1;
            }
            api->packet_unref(pkt);
            continue;
        }
        if (pkt->size > 0 && enc->mux) {
            // Straight into the muxer.  It gets the packet's own timeline
            // and its key-frame flag, so nothing is reassembled here: that
            // is what keeps the container's parameter sets and sample table
            // consistent with the bitstream itself.
            //
            // The stream's time base is not the one asked for, though:
            // avformat_write_header settles it (movenc normalised our
            // 1/1000 into 1/16000), and packet timestamps are read in *that*
            // unit.  Unscaled millisecond values made a three-second
            // recording play in 0.18s, so the millisecond timeline is
            // rescaled into the muxer's time base here.
            AVRational ms = {1, 1000};
            // The frame this packet belongs to, whose duration was queued on
            // its way in — not the frame being sent right now, which is one
            // or two ahead of it.
            int64_t duration = pop_duration(enc);
            pkt->stream_index = enc->mux_stream->index;
            pkt->pts = pkt->dts = api->rescale_q(enc->mux_next_pts, ms, enc->mux_time_base);
            pkt->duration = api->rescale_q(duration, ms, enc->mux_time_base);
            enc->mux_next_pts += duration;
            enc->mux_frames++;
            int written = api->format_write_frame(enc->mux, pkt);
            if (written < 0) {
                set_err(enc, "writing a packet to the MP4 muxer failed", written);
                api->packet_free(&pkt);
                return -1;
            }
            api->packet_unref(pkt);
            continue;
        }
        if (pkt->size > 0) {
            size_t needed = enc->out_len + (size_t)pkt->size;
            if (needed > enc->out_cap) {
                size_t cap = enc->out_cap ? enc->out_cap : 65536;
                while (cap < needed) {
                    cap *= 2;
                }
                uint8_t *grown = realloc(enc->out, cap);
                if (!grown) {
                    snprintf(enc->err, sizeof(enc->err), "out of memory growing the output");
                    api->packet_free(&pkt);
                    return -1;
                }
                enc->out = grown;
                enc->out_cap = cap;
            }
            memcpy(enc->out + enc->out_len, pkt->data, (size_t)pkt->size);
            enc->out_len += (size_t)pkt->size;
        }
        api->packet_unref(pkt);
    }
    api->packet_free(&pkt);
    return 0;
}

// RGBA -> NV12, BT.601 limited range, into the reused software frame.
// Straight scalar code: at 4K this is a few milliseconds on the compatibility
// path, which the zero-copy path avoids entirely.
static void convert_rgba_to_nv12(uint8_t *y_plane, uint8_t *uv_plane, int width, int height,
                                 const uint8_t *rgba) {
    for (int row = 0; row < height; row++) {
        const uint8_t *src = rgba + (size_t)row * width * 4;
        uint8_t *dst = y_plane + (size_t)row * width;
        for (int x = 0; x < width; x++) {
            int r = src[x * 4 + 0];
            int g = src[x * 4 + 1];
            int b = src[x * 4 + 2];
            int luma = (66 * r + 129 * g + 25 * b + 128) >> 8;
            dst[x] = (uint8_t)(luma + 16 < 0 ? 0 : (luma + 16 > 255 ? 255 : luma + 16));
        }
    }
    for (int row = 0; row < height / 2; row++) {
        const uint8_t *row0 = rgba + (size_t)(row * 2) * width * 4;
        const uint8_t *row1 = rgba + (size_t)(row * 2 + 1) * width * 4;
        uint8_t *dst = uv_plane + (size_t)row * width;
        for (int x = 0; x < width / 2; x++) {
            int r = (row0[x * 8 + 0] + row0[x * 8 + 4] + row1[x * 8 + 0] + row1[x * 8 + 4] + 2) / 4;
            int g =
                (row0[x * 8 + 1] + row0[x * 8 + 5] + row1[x * 8 + 1] + row1[x * 8 + 5] + 2) / 4;
            int b =
                (row0[x * 8 + 2] + row0[x * 8 + 6] + row1[x * 8 + 2] + row1[x * 8 + 6] + 2) / 4;
            int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            dst[x * 2 + 0] = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            dst[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

// The encoder name for a short codec word: "h264" -> "h264_vaapi" on the VAAPI
// backend, "h264" -> "h264_nvenc" on NVENC.  The caller passes one of the names
// `vshot record --encoder` accepts.
static const char *backend_encoder_name(int backend, const char *codec, char *scratch,
                                        size_t scratch_size) {
    snprintf(scratch, scratch_size, "%s_%s", codec,
             backend == VSHOT_BACKEND_NVENC ? "nvenc" : "vaapi");
    return scratch;
}

// The pixel format the encoder takes on each backend: a VAAPI surface, or a
// CUDA device pointer.
static enum AVPixelFormat backend_pix_fmt(int backend) {
    return backend == VSHOT_BACKEND_NVENC ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
}

static int open_encoder(VshotAvEnc *enc, int width, int height, const char *codec, int qp,
                        int gop_frames) {
    // The hardware device: a render node for VAAPI, a CUDA device for NVENC.
    // NVENC's device is named by index (the ffmpeg CUDA hwcontext counts
    // devices); `VSHOT_NVENC_DEVICE` overrides it for a multi-GPU machine.
    int ret;
    if (enc->backend == VSHOT_BACKEND_NVENC) {
        const char *index = getenv("VSHOT_NVENC_DEVICE");
        ret = api->hwdevice_ctx_create(&enc->device, AV_HWDEVICE_TYPE_CUDA, index, NULL, 0);
        if (ret < 0) {
            set_err(enc, "no CUDA device could be opened for NVENC encoding", ret);
            return -1;
        }
    } else {
        ret = api->hwdevice_ctx_create(&enc->device, AV_HWDEVICE_TYPE_VAAPI,
                                       "/dev/dri/renderD128", NULL, 0);
        if (ret < 0) {
            set_err(enc, "no VAAPI device could be opened for encoding", ret);
            return -1;
        }
    }
    char name[64];
    const AVCodec *encoder =
        api->find_encoder_by_name(backend_encoder_name(enc->backend, codec, name, sizeof(name)));
    if (!encoder) {
        snprintf(enc->err, sizeof(enc->err), "this ffmpeg build has no %s encoder", name);
        return -1;
    }
    enc->ctx = api->alloc_context3(encoder);
    if (!enc->ctx) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the encoder context");
        return -1;
    }
    enc->ctx->width = width;
    enc->ctx->height = height;
    enc->ctx->time_base = (AVRational){1, 1000000};
    enc->ctx->framerate = (AVRational){0, 1};
    enc->ctx->pix_fmt = backend_pix_fmt(enc->backend);
    // Every frame its own IDR: this encoder only produces I frames when it
    // is told at open time (the radeonsi VCN ignores mid-stream requests),
    // and idr_interval counts *I frames*, so any interval above 0 produces
    // a stream whose only key frame is the first — unseekable and
    // uncuttable.  An all-intra stream is what the direct-libva path
    // produced too; the bitrate is higher, every frame decodes on its own.
    //
    // A replay wants the opposite: a bounded GOP (`gop_frames` > 0) so a
    // window of history costs a fraction of an all-intra stream and every
    // GOP boundary is a place a save can start from.  `idr_interval` stays 0
    // for the recording case (all-intra) and is set to the GOP length for a
    // replay, which makes the encoder emit a key frame every `gop_frames`.
    enc->ctx->gop_size = gop_frames > 0 ? gop_frames : 0;
    // Ask libavcodec for the parameter sets as extradata (SPS/PPS for
    // H.264, VPS/SPS/PPS for HEVC, the sequence header OBU for AV1)
    // instead of leaving the muxer to pick them out of the stream.  This
    // is exactly what an MP4 muxer built on libavcodec does:
    // avcodec_parameters_from_context carries the extradata into the
    // sample entry's configuration record.
    enc->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    // The level chosen only for H.264: 6.2 covers everything up to vshot's
    // 8192-pixel limit (a two-screen composed desktop at scale 2 easily
    // passes level 5.1's 4096-wide cap).  HEVC and AV1 pick their level
    // automatically, which is also what a future profile change wants.
    if (strcmp(codec, "h264") == 0) {
        enc->ctx->level = 62;
    }
    enc->ctx->bit_rate = 0;
    // The rate-control options are the encoders' private options (they
    // live in ctx->priv_data); AV_OPT_SEARCH_CHILDREN is how libavcodec
    // itself reaches them from the context.  Every option is checked: an
    // unrecognised name fails silently otherwise, and a stream without
    // key frames is the exact bug a wrong name produced here once.
    //
    // The names differ by backend.  VAAPI's are `rc_mode=CQP` (a string) and
    // `idr_interval` for the key-frame distance (`g` is the generic name and
    // these encoders do not take it — the first recordings had no IDR at all
    // because of that).  NVENC's are `rc=constqp` (an int, so it is set from
    // the enum's numeric value) and the generic `g`, with `forced-idr` so a
    // key frame is a real IDR.  The key-frame distance itself is carried by
    // `ctx->gop_size` on both (0 = every frame an I frame), which NVENC reads
    // through its `g` option and VAAPI through `idr_interval`.
    if (enc->backend == VSHOT_BACKEND_NVENC) {
        // NVENC_RC_CONSTQP == 0 in the encoder's own enum; the string is not
        // accepted for an integer AVOption.
        if (api->opt_set_int(enc->ctx, "rc", 0, AV_OPT_SEARCH_CHILDREN) < 0) {
            snprintf(enc->err, sizeof(enc->err),
                     "this ffmpeg build's %s_nvenc encoder does not take the constqp "
                     "rate-control option",
                     codec);
            return -1;
        }
        api->opt_set_int(enc->ctx, "forced-idr", 1, AV_OPT_SEARCH_CHILDREN);
    } else if (api->opt_set(enc->ctx, "rc_mode", "CQP", AV_OPT_SEARCH_CHILDREN) < 0 ||
               api->opt_set_int(enc->ctx, "idr_interval", 0, AV_OPT_SEARCH_CHILDREN) < 0) {
        snprintf(enc->err, sizeof(enc->err),
                 "this ffmpeg build's %s encoder does not take the CQP rate-control options",
                 codec);
        return -1;
    }
    // The quality knob is per-encoder.  H.264 and HEVC carry a private `qp`
    // option on both backends, which is what libavcodec's `explicit_qp`
    // reads.  The AV1 encoder has no such option and takes its CQP level from
    // the generic `global_quality` field instead — exactly what ffmpeg's own
    // `-qp` sets.  Requiring `qp` from every encoder is what made
    // `--encoder av1` fail to open.
    if (api->opt_set_int(enc->ctx, "qp", qp, AV_OPT_SEARCH_CHILDREN) < 0) {
        enc->ctx->global_quality = qp;
    }
    // Keep the stream simple and seekable: no B frames.  `bf` is a public
    // AVCodecContext option (not private), and AV1 has no B frames and no
    // such option.
    if (strcmp(codec, "av1") != 0 && api->opt_set_int(enc->ctx, "bf", 0, 0) < 0) {
        snprintf(enc->err, sizeof(enc->err),
                 "the %s encoder on this ffmpeg build takes no bf option", codec);
        return -1;
    }
    // How many pictures the encoder keeps in flight.  The default (2) leaves
    // the zero-copy chain starved at 4K: the filtergraph's `scale_vaapi`
    // blocks on an output surface the encoder has not returned yet, which at
    // 4K costs ~19 ms on a fraction of frames — enough, against a 16.7 ms
    // budget, to make a 4K60 session run at ~58 fps (measured).  A deeper
    // queue lets the conversion run ahead of the encoder, and the stall goes
    // away.  The option is private to the VAAPI encoders: NVENC has no
    // `async_depth` (its own knobs are `surfaces` and `delay`), so the call
    // below fails there and the default stays.  The failure is deliberately
    // ignored — a build without the option keeps its own default either way.
    api->opt_set_int(enc->ctx, "async_depth", 8, AV_OPT_SEARCH_CHILDREN);
    // The colour properties the zero-copy chain carries: the packed RGB
    // the compositor produces is full-range BT.709.  These match the
    // parameters wf-recorder records with on this hardware.
    api->opt_set_int(enc->ctx, "color_range", 2, 0);      // AVCOL_RANGE_JPEG
    api->opt_set_int(enc->ctx, "colorspace", 1, 0);       // AVCOL_SPC_BT709
    api->opt_set_int(enc->ctx, "color_primaries", 1, 0);  // AVCOL_PRI_BT709
    api->opt_set_int(enc->ctx, "color_trc", 1, 0);        // AVCOL_TRC_BT709
    return 0;
}

// The dma-buf format the buffer came with, as the sw_format the VAAPI
// frames context must declare.  Only the two packed formats screencopy
// uses are accepted; anything else is refused by name.
static int fourcc_to_sw_format(unsigned fourcc) {
    switch (fourcc) {
    case 0x34325241: // DRM_FORMAT_ARGB8888
        return AV_PIX_FMT_BGRA;
    case 0x34325258: // DRM_FORMAT_XRGB8888
        return AV_PIX_FMT_BGR0;
    default:
        return -1;
    }
}

// Builds the filtergraph the zero-copy path runs: VAAPI in (the mapped
// dma-buf surfaces) -> scale_vaapi=format=nv12 -> VAAPI out (what the
// encoder takes).  The GPU does the colour conversion; no pixels are
// touched on the CPU.
//
// `in_w`/`in_h` are the input frames' size, which is not always the
// encoder's: `record window` rebuilds this graph when the window is resized
// mid-recording.  When `fit_w`/`fit_h` are positive the graph fits the
// input into that box — scaled down until it fits, never up, then
// letterboxed — so the encoder's own canvas stays the size it was opened
// for.  The open-time call passes the encoder's size and no fit box.
//
// The letterbox is composed with `overlay_vaapi` over a black plate, and the
// plate is an input the caller feeds with every frame (see
// `vshot_av_enc_send_dmabuf`).  `pad_vaapi` would be the two-filter way to
// write those bars, but it only writes where it draws: on radeonsi the
// padded region keeps whatever the output surface held before, and the
// surfaces a rebuilt fit chain draws into are exactly the ones the previous
// chain used — so the bars replay the old size's pixels (measured: a window
// resized smaller left its old content in the letterbox).  overlay_vaapi
// rewrites the whole canvas — main first, blended content second — so the
// bars are the plate's colour by construction, every frame, whatever the
// surface used to hold.
//
// The plate itself: a canvas-sized VAAPI pool in the packed format and one
// black frame in it, uploaded once.  Every capture frame re-sends a
// reference to it, so the composed canvas has a defined background at every
// pixel the content does not reach.  The alpha byte is 255 — the plate is
// the background it is composed over.
static int build_plate(VshotAvEnc *enc, int width, int height) {
    enc->plate_frames = api->hwframe_ctx_alloc(enc->device);
    if (!enc->plate_frames) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the plate's frame pool");
        return -1;
    }
    AVHWFramesContext *plate_pool = (AVHWFramesContext *)enc->plate_frames->data;
    plate_pool->format = AV_PIX_FMT_VAAPI;
    plate_pool->sw_format = AV_PIX_FMT_BGRA;
    plate_pool->width = width;
    plate_pool->height = height;
    int ret = api->hwframe_ctx_init(enc->plate_frames);
    if (ret < 0) {
        set_err(enc, "could not initialise the plate's frame pool", ret);
        return -1;
    }
    enc->plate = api->frame_alloc();
    AVFrame *sw = api->frame_alloc();
    if (!enc->plate || !sw) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the plate frame");
        api->frame_free(&enc->plate);
        api->frame_free(&sw);
        return -1;
    }
    ret = api->hwframe_get_buffer(enc->plate_frames, enc->plate, 0);
    if (ret < 0) {
        set_err(enc, "could not allocate the plate's surface", ret);
        api->frame_free(&enc->plate);
        api->frame_free(&sw);
        return -1;
    }
    sw->format = AV_PIX_FMT_BGRA;
    sw->width = width;
    sw->height = height;
    ret = api->frame_get_buffer(sw, 0);
    if (ret < 0) {
        set_err(enc, "could not allocate the plate's pixels", ret);
        api->frame_free(&enc->plate);
        api->frame_free(&sw);
        return -1;
    }
    for (int y = 0; y < height; y++) {
        uint8_t *row = sw->data[0] + (size_t)y * sw->linesize[0];
        for (int x = 0; x < width; x++) {
            row[x * 4 + 0] = 0;
            row[x * 4 + 1] = 0;
            row[x * 4 + 2] = 0;
            row[x * 4 + 3] = 255;
        }
    }
    ret = api->hwframe_transfer_data(enc->plate, sw, 0);
    api->frame_free(&sw);
    if (ret < 0) {
        set_err(enc, "could not upload the plate", ret);
        api->frame_free(&enc->plate);
        return -1;
    }
    return 0;
}

static int open_filtergraph_dmabuf_fit(VshotAvEnc *enc, int sw_format, int in_w, int in_h,
                                       int fit_w, int fit_h);

// Tears down whatever chain this session currently has: the filtergraph, the
// fit chain's plate (frame and pool) and the dma-buf input pool.  A rebuild
// replaces all of it together.
static void teardown_graph(VshotAvEnc *enc) {
    if (enc->graph) {
        api->filter_graph_free(&enc->graph);
    }
    enc->graph = NULL;
    enc->graph_src = NULL;
    enc->graph_sink = NULL;
    enc->graph_plate = NULL;
    if (enc->plate) {
        api->frame_free(&enc->plate);
    }
    if (enc->plate_frames) {
        api->buffer_unref(&enc->plate_frames);
    }
    if (enc->frames) {
        api->buffer_unref(&enc->frames);
    }
}

// Builds one chain.  `use_overlay` is only meaningful when fitting: 1 builds
// the plate + overlay chain, 0 the pad chain (the fallback for drivers whose
// overlay_vaapi refuses to configure).  On failure the caller tears down
// whatever was built.
static int build_fit_graph(VshotAvEnc *enc, int sw_format, int in_w, int in_h, int fit_w,
                           int fit_h, int use_overlay) {
    int overlay = use_overlay && fit_w > 0 && fit_h > 0;
    if (!api->filter_loaded) {
        snprintf(enc->err, sizeof(enc->err),
                 "libavfilter is not available, so zero-copy recording cannot run "
                 "(install ffmpeg)");
        return -1;
    }
    if (!enc->device) {
        snprintf(enc->err, sizeof(enc->err), "the VAAPI device is not open");
        return -1;
    }
    // The input pool: VAAPI frames in the buffer's own packed format.
    // Mapping a dma-buf needs a frames context to map into.
    enc->frames = api->hwframe_ctx_alloc(enc->device);
    if (!enc->frames) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the input frame pool");
        return -1;
    }
    AVHWFramesContext *frames = (AVHWFramesContext *)enc->frames->data;
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = sw_format;
    frames->width = in_w;
    frames->height = in_h;
    int ret = api->hwframe_ctx_init(enc->frames);
    if (ret < 0) {
        set_err(enc, "could not initialise the input frame pool", ret);
        return -1;
    }

    enc->graph = api->filter_graph_alloc();
    if (!enc->graph) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the filter graph");
        return -1;
    }
    const AVFilter *source = api->filter_get_by_name("buffer");
    const AVFilter *sink = api->filter_get_by_name("buffersink");
    if (!source || !sink) {
        snprintf(enc->err, sizeof(enc->err), "the buffer or buffersink filter is missing");
        return -1;
    }
    enc->graph_src = api->filter_graph_alloc_filter(enc->graph, source, "Source");
    if (!enc->graph_src) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the graph's source filter");
        return -1;
    }
    AVBufferSrcParameters *params = api->buffersrc_parameters_alloc();
    if (!params) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the source filter parameters");
        return -1;
    }
    memset(params, 0, sizeof(*params));
    params->format = AV_PIX_FMT_NONE;
    params->hw_frames_ctx = enc->frames;
    ret = api->buffersrc_parameters_set(enc->graph_src, params);
    free(params); // av_free is libc free with the default allocator
    if (ret < 0) {
        set_err(enc, "could not set the source filter's frames context", ret);
        return -1;
    }
    char config[128];
    snprintf(config, sizeof(config),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1", in_w, in_h,
             (int)AV_PIX_FMT_VAAPI);
    ret = api->filter_init_str(enc->graph_src, config);
    if (ret < 0) {
        set_err(enc, "could not initialise the source filter", ret);
        return -1;
    }
    // The fit chain needs a second input, the black plate the letterbox is
    // composed over.  It is a canvas-sized VAAPI pool of its own; the plate
    // frame in it is uploaded once (build_plate) and re-sent with every
    // capture frame.
    if (overlay) {
        if (build_plate(enc, fit_w, fit_h) != 0) {
            return -1;
        }
        enc->graph_plate = api->filter_graph_alloc_filter(enc->graph, source, "Plate");
        if (!enc->graph_plate) {
            snprintf(enc->err, sizeof(enc->err), "could not allocate the plate's source filter");
            return -1;
        }
        AVBufferSrcParameters *plate_params = api->buffersrc_parameters_alloc();
        if (!plate_params) {
            snprintf(enc->err, sizeof(enc->err),
                     "could not allocate the plate source's parameters");
            return -1;
        }
        memset(plate_params, 0, sizeof(*plate_params));
        plate_params->format = AV_PIX_FMT_NONE;
        plate_params->hw_frames_ctx = enc->plate_frames;
        ret = api->buffersrc_parameters_set(enc->graph_plate, plate_params);
        free(plate_params);
        if (ret < 0) {
            set_err(enc, "could not set the plate source's frames context", ret);
            return -1;
        }
        snprintf(config, sizeof(config),
                 "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1", fit_w, fit_h,
                 (int)AV_PIX_FMT_VAAPI);
        ret = api->filter_init_str(enc->graph_plate, config);
        if (ret < 0) {
            set_err(enc, "could not initialise the plate source filter", ret);
            return -1;
        }
    }
    enc->graph_sink = api->filter_graph_alloc_filter(enc->graph, sink, "Sink");
    if (!enc->graph_sink) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the graph's sink filter");
        return -1;
    }
    ret = api->opt_set(enc->graph_sink, "pixel_formats", "vaapi", AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        set_err(enc, "could not set the sink's pixel formats", ret);
        return -1;
    }
    ret = api->filter_init_dict(enc->graph_sink, NULL);
    if (ret < 0) {
        set_err(enc, "could not initialise the sink filter", ret);
        return -1;
    }
    AVFilterInOut *outputs = api->filter_inout_alloc();
    AVFilterInOut *inputs = api->filter_inout_alloc();
    if (!outputs || !inputs) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the graph's in/out lists");
        api->filter_inout_free(&inputs);
        api->filter_inout_free(&outputs);
        return -1;
    }
    outputs->name = api->strdup("in");
    outputs->filter_ctx = enc->graph_src;
    outputs->pad_idx = 0;
    outputs->next = NULL;
    if (overlay) {
        // The plate is the chain's second source: its own buffer filter,
        // fed the black plate frame with every capture frame.
        AVFilterInOut *plate_link = api->filter_inout_alloc();
        if (!plate_link) {
            snprintf(enc->err, sizeof(enc->err), "could not allocate the plate's in/out link");
            api->filter_inout_free(&inputs);
            api->filter_inout_free(&outputs);
            return -1;
        }
        plate_link->name = api->strdup("plate");
        plate_link->filter_ctx = enc->graph_plate;
        plate_link->pad_idx = 0;
        plate_link->next = NULL;
        outputs->next = plate_link;
    }
    inputs->name = api->strdup("out");
    inputs->filter_ctx = enc->graph_sink;
    inputs->pad_idx = 0;
    inputs->next = NULL;
    char chain[512];
    if (fit_w > 0 && fit_h > 0) {
        // Fit the new-size frame into the canvas: scaled down when it is
        // larger than the box, kept at its own size and centred when it is
        // smaller (never upscaled — a recording does not invent pixels).
        int scaled_w = in_w;
        int scaled_h = in_h;
        if (in_w > fit_w || in_h > fit_h) {
            double scale = (double)fit_w / (double)in_w;
            double by_height = (double)fit_h / (double)in_h;
            if (by_height < scale) {
                scale = by_height;
            }
            scaled_w = ((int)((double)in_w * scale)) & ~1;
            scaled_h = ((int)((double)in_h * scale)) & ~1;
            if (scaled_w < 2) {
                scaled_w = 2;
            }
            if (scaled_h < 2) {
                scaled_h = 2;
            }
        }
        if (overlay) {
            // The scaled frame carries no alpha (`bgr0`): overlay_vaapi
            // treats an overlay input with an alpha channel as
            // premultiplied, and a capture buffer's alpha byte is whatever
            // the compositor left there — zero for a fully occluding window
            // — which would erase the content from the composed frame.
            // Dropping the byte keeps the pixels; the plate underneath
            // supplies the background.
            snprintf(chain, sizeof(chain),
                     "[in]scale_vaapi=w=%d:h=%d:format=bgr0[content];"
                     "[plate]null[bg];"
                     "[bg][content]overlay_vaapi=x=(main_w-overlay_w)/2:y=(main_h-"
                     "overlay_h)/2[comp];"
                     "[comp]scale_vaapi=format=nv12:out_range=full[out]",
                     scaled_w, scaled_h);
        } else {
            // The fallback composition, for drivers whose overlay_vaapi will
            // not configure: scale to fit, then pad the rest of the canvas.
            // The fill lands in the packed surface before the NV12
            // conversion (padding after it would fill the planes with YUV
            // zeros, which a full-range player decodes as dark green), but
            // pad only writes where it draws — see the note above.
            const char *packed = sw_format == AV_PIX_FMT_BGRA ? "bgra" : "bgr0";
            snprintf(chain, sizeof(chain),
                     "[in]scale_vaapi=w=%d:h=%d:format=%s[content];"
                     "[content]pad_vaapi=w=%d:h=%d:x=(ow-iw)/2:y=(oh-ih)/2,"
                     "scale_vaapi=format=nv12:out_range=full[out]",
                     scaled_w, scaled_h, packed, fit_w, fit_h);
        }
    } else {
        snprintf(chain, sizeof(chain), "[in]scale_vaapi=format=nv12:out_range=full[out]");
    }
    ret = api->filter_graph_parse_ptr(enc->graph, chain, &inputs, &outputs, NULL);
    api->filter_inout_free(&inputs);
    api->filter_inout_free(&outputs);
    if (ret < 0) {
        set_err(enc, "could not build the filtergraph", ret);
        return -1;
    }
    // Filters that touch hardware frames need the device on their own
    // context; parse_ptr cannot wire that up itself.
    for (unsigned i = 0; i < enc->graph->nb_filters; i++) {
        enc->graph->filters[i]->hw_device_ctx = api->buffer_ref(enc->device);
    }
    ret = api->filter_graph_config(enc->graph, NULL);
    if (ret < 0) {
        set_err(enc, "could not configure the filtergraph", ret);
        return -1;
    }
    return 0;
}

// The chain the encoder runs: the overlay composition for a fit, the
// two-filter conversion otherwise.
//
// The overlay chain is preferred where it works because it *writes* the
// letterbox (see above); a driver whose VAAPI video processor cannot blend
// refuses to configure it — `overlay_vaapi` needs VA_BLEND_GLOBAL_ALPHA —
// and the pad chain is the one that has always been here, so it is the
// fallback rather than the only route.
static int open_filtergraph_dmabuf_fit(VshotAvEnc *enc, int sw_format, int in_w, int in_h,
                                       int fit_w, int fit_h) {
    if (fit_w <= 0 || fit_h <= 0) {
        return build_fit_graph(enc, sw_format, in_w, in_h, fit_w, fit_h, 0);
    }
    // A test hook for the fallback: the machines this was developed on all
    // blend, so the pad chain would otherwise never run in a test.
    if (getenv("VSHOT_RECORD_NO_OVERLAY")) {
        return build_fit_graph(enc, sw_format, in_w, in_h, fit_w, fit_h, 0);
    }
    if (build_fit_graph(enc, sw_format, in_w, in_h, fit_w, fit_h, 1) == 0) {
        return 0;
    }
    if (trace_enabled()) {
        fprintf(stderr,
                "vshot:   the overlay fit chain did not configure (%s); falling back to the "
                "pad chain\n",
                enc->err);
    }
    teardown_graph(enc);
    return build_fit_graph(enc, sw_format, in_w, in_h, fit_w, fit_h, 0);
}

// The open-time graph: the input pool is the output pool.
static int open_filtergraph_dmabuf(VshotAvEnc *enc, int sw_format) {
    return open_filtergraph_dmabuf_fit(enc, sw_format, enc->width, enc->height, 0, 0);
}

// Opens a session whose factory is done by the caller: the software path
// calls this with `dmabuf == 0` after sizing `enc->sw`, the zero-copy path
// with `dmabuf == 1` and the buffer's fourcc.
static int finish_open(VshotAvEnc *enc, int width, int height, const char *codec, int qp,
                       int dmabuf, unsigned fourcc, int gop_frames) {
    enc->width = width;
    enc->height = height;
    enc->in_w = width;
    enc->in_h = height;
    enc->dmabuf = dmabuf;
    enc->fourcc = fourcc;
    if (open_encoder(enc, width, height, codec, qp, gop_frames) != 0) {
        return -1;
    }
    int ret;
    if (dmabuf && enc->backend == VSHOT_BACKEND_NVENC) {
        // NVENC has no dma-buf import: ffmpeg's CUDA hwcontext maps only CUDA
        // device memory and CUDA arrays, never an AV_PIX_FMT_DRM_PRIME frame,
        // so a Wayland capture buffer cannot be handed to it without a copy
        // through system memory.  The caller asks for the software path
        // instead (see `Recorder::open` on the Rust side); reaching here with
        // `dmabuf` set is a bug worth naming rather than a crash to debug.
        snprintf(enc->err, sizeof(enc->err),
                 "the NVENC backend cannot import a compositor dma-buf; it records the "
                 "software path");
        return -1;
    }
    if (dmabuf) {
        int sw_format = fourcc_to_sw_format(fourcc);
        if (sw_format < 0) {
            snprintf(enc->err, sizeof(enc->err),
                     "the dma-buf format 0x%08x is not a packed RGB format the encoder accepts",
                     fourcc);
            return -1;
        }
        if (open_filtergraph_dmabuf(enc, sw_format) != 0) {
            return -1;
        }
        // The encoder takes what the filtergraph hands out.
        AVFilterLink *link = enc->graph_sink->inputs[0];
        enc->ctx->width = link->w;
        enc->ctx->height = link->h;
        enc->ctx->pix_fmt = link->format;
        enc->ctx->time_base = link->time_base;
        AVBufferRef *sink_frames = api->buffersink_get_hw_frames_ctx(enc->graph_sink);
        if (sink_frames) {
            enc->ctx->hw_frames_ctx = api->buffer_ref(sink_frames);
        }
    } else {
        enc->frames = api->hwframe_ctx_alloc(enc->device);
        if (!enc->frames) {
            snprintf(enc->err, sizeof(enc->err), "could not allocate the frame pool");
            return -1;
        }
        AVHWFramesContext *frames = (AVHWFramesContext *)enc->frames->data;
        frames->format = backend_pix_fmt(enc->backend);
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = width;
        frames->height = height;
        frames->initial_pool_size = 0;
        ret = api->hwframe_ctx_init(enc->frames);
        if (ret < 0) {
            set_err(enc, "could not initialise the hardware frame pool", ret);
            return -1;
        }
        enc->ctx->hw_frames_ctx = api->buffer_ref(enc->frames);
    }
    char name[64];
    const AVCodec *encoder =
        api->find_encoder_by_name(backend_encoder_name(enc->backend, codec, name, sizeof(name)));
    ret = api->open2(enc->ctx, encoder, NULL);
    if (ret < 0) {
        set_err(enc, "opening the encoder failed", ret);
        return -1;
    }
    if (!dmabuf) {
        enc->sw = api->frame_alloc();
        if (!enc->sw) {
            snprintf(enc->err, sizeof(enc->err), "could not allocate the NV12 staging frame");
            return -1;
        }
        enc->sw->format = AV_PIX_FMT_NV12;
        enc->sw->width = width;
        enc->sw->height = height;
        enc->sw->linesize[0] = width;
        enc->sw->linesize[1] = width;
        enc->sw->data[0] = malloc((size_t)width * height);
        enc->sw->data[1] = malloc((size_t)width * (height / 2));
        if (!enc->sw->data[0] || !enc->sw->data[1]) {
            snprintf(enc->err, sizeof(enc->err), "out of memory for the NV12 staging frame");
            return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The exported interface
// ---------------------------------------------------------------------------

VshotAvEnc *vshot_av_enc_create(int width, int height, const char *codec, int qp, int gop_frames,
                                int backend) {
    api = load_api();
    if (!api) {
        return NULL;
    }
    if (width <= 0 || height <= 0 || !codec || !codec[0]) {
        return NULL;
    }
    // Quiet libav* before anything opens: the encoder says things at info
    // level ("No quality level set", profile notices) that would otherwise
    // land in the middle of vshot's own stderr.  The debug switch keeps
    // them, which is what it is for.
    if (getenv("VSHOT_RECORD_DEBUG")) {
        api->log_set_level(AV_LOG_INFO);
    } else {
        api->log_set_level(AV_LOG_ERROR);
    }
    VshotAvEnc *enc = calloc(1, sizeof(VshotAvEnc));
    if (!enc) {
        return NULL;
    }
    enc->backend = backend;
    if (finish_open(enc, width, height, codec, qp, 0, 0, gop_frames) != 0) {
        snprintf(create_error, sizeof(create_error), "%s", enc->err);
        vshot_av_enc_destroy(enc);
        return NULL;
    }
    return enc;
}

VshotAvEnc *vshot_av_enc_create_dmabuf(int width, int height, const char *codec, int qp,
                                       int gop_frames, unsigned fourcc, int backend) {
    api = load_api();
    if (!api) {
        return NULL;
    }
    if (width <= 0 || height <= 0 || !codec || !codec[0]) {
        return NULL;
    }
    if (getenv("VSHOT_RECORD_DEBUG")) {
        api->log_set_level(AV_LOG_INFO);
    } else {
        api->log_set_level(AV_LOG_ERROR);
    }
    VshotAvEnc *enc = calloc(1, sizeof(VshotAvEnc));
    if (!enc) {
        return NULL;
    }
    enc->backend = backend;
    if (finish_open(enc, width, height, codec, qp, 1, fourcc, gop_frames) != 0) {
        snprintf(create_error, sizeof(create_error), "%s", enc->err);
        vshot_av_enc_destroy(enc);
        return NULL;
    }
    return enc;
}

void vshot_av_enc_destroy(VshotAvEnc *enc) {
    if (!enc) {
        return;
    }
    for (int i = 0; i < enc->map_count; i++) {
        if (enc->maps[i].frame && api) {
            api->frame_unref(enc->maps[i].frame);
            api->frame_free(&enc->maps[i].frame);
        }
    }
    if (enc->sw) {
        free(enc->sw->data[0]);
        free(enc->sw->data[1]);
        if (api) {
            api->frame_free(&enc->sw);
        }
    }
    if (enc->plate && api) {
        api->frame_free(&enc->plate);
    }
    if (enc->plate_frames && api) {
        api->buffer_unref(&enc->plate_frames);
    }
    if (enc->graph && api) {
        api->filter_graph_free(&enc->graph);
    }
    if (enc->ctx && api) {
        api->free_context(&enc->ctx);
    }
    if (enc->frames && api) {
        api->buffer_unref(&enc->frames);
    }
    if (enc->device && api) {
        api->buffer_unref(&enc->device);
    }
    if (enc->replay && api) {
        replay_destroy(enc->replay);
        enc->replay = NULL;
    }
    free(enc->sw_fit);
    free(enc->out);
    free(enc);
}

// Fits one source RGBA frame into the encoder's canvas on the CPU: scaled
// down when it is larger than the canvas, centred at its own size when it is
// smaller, over a black background.  This is the software path's answer to the
// dma-buf chain's `scale_vaapi` + letterbox: `record window` can be resized
// mid-recording, one file holds one frame size, so the new frames are fitted
// into the size the file was opened with.
//
// A frame already at the canvas size is returned as it is — the common case,
// and the one that must not pay for a copy.
static const uint8_t *fit_rgba_to_canvas(VshotAvEnc *enc, const uint8_t *rgba) {
    int canvas_w = enc->width;
    int canvas_h = enc->height;
    if (enc->in_w == canvas_w && enc->in_h == canvas_h) {
        return rgba;
    }
    size_t needed = (size_t)canvas_w * (size_t)canvas_h * 4;
    if (enc->sw_fit == NULL || enc->sw_fit_cap < needed) {
        uint8_t *grown = realloc(enc->sw_fit, needed);
        if (grown == NULL) {
            snprintf(enc->err, sizeof(enc->err), "out of memory for the fit staging buffer");
            return NULL;
        }
        enc->sw_fit = grown;
        enc->sw_fit_cap = needed;
    }
    // The same arithmetic the fit graph uses: scale down until it fits, never
    // up, and keep the result even-sized (a NV12 chroma plane is half size).
    int scaled_w = enc->in_w;
    int scaled_h = enc->in_h;
    if (enc->in_w > canvas_w || enc->in_h > canvas_h) {
        double scale = (double)canvas_w / (double)enc->in_w;
        double by_height = (double)canvas_h / (double)enc->in_h;
        if (by_height < scale) {
            scale = by_height;
        }
        scaled_w = ((int)((double)enc->in_w * scale)) & ~1;
        scaled_h = ((int)((double)enc->in_h * scale)) & ~1;
        if (scaled_w < 2) {
            scaled_w = 2;
        }
        if (scaled_h < 2) {
            scaled_h = 2;
        }
    }
    int offset_x = (canvas_w - scaled_w) / 2;
    int offset_y = (canvas_h - scaled_h) / 2;
    // Black bars, opaque: the plate the dma-buf chain composes over.
    for (size_t i = 0; i < (size_t)canvas_w * (size_t)canvas_h; i++) {
        enc->sw_fit[i * 4 + 0] = 0;
        enc->sw_fit[i * 4 + 1] = 0;
        enc->sw_fit[i * 4 + 2] = 0;
        enc->sw_fit[i * 4 + 3] = 255;
    }
    // Nearest-neighbour row copy: the recording is a record of what was on
    // screen, and this path exists for compatibility, not for resampling
    // quality.  A row that lands outside the canvas (a scaled size that
    // rounded up by a pixel) is skipped.
    for (int y = 0; y < scaled_h; y++) {
        int destination_y = offset_y + y;
        if (destination_y < 0 || destination_y >= canvas_h) {
            continue;
        }
        int source_y = (int)((long)y * enc->in_h / scaled_h);
        if (source_y >= enc->in_h) {
            source_y = enc->in_h - 1;
        }
        const uint8_t *source_row = rgba + (size_t)source_y * (size_t)enc->in_w * 4;
        uint8_t *destination_row =
            enc->sw_fit + ((size_t)destination_y * (size_t)canvas_w + (size_t)offset_x) * 4;
        for (int x = 0; x < scaled_w; x++) {
            int source_x = (int)((long)x * enc->in_w / scaled_w);
            if (source_x >= enc->in_w) {
                source_x = enc->in_w - 1;
            }
            const uint8_t *source_pixel = source_row + (size_t)source_x * 4;
            destination_row[x * 4 + 0] = source_pixel[0];
            destination_row[x * 4 + 1] = source_pixel[1];
            destination_row[x * 4 + 2] = source_pixel[2];
            destination_row[x * 4 + 3] = 255;
        }
    }
    return enc->sw_fit;
}

// Sends one RGBA frame; the packets it produces are accumulated for `take`.
// Returns 0 on success, -1 with `last_error` set otherwise.
int vshot_av_enc_send(VshotAvEnc *enc, const uint8_t *rgba) {
    enc->out_len = 0;
    if (!api || !enc || !enc->ctx) {
        return -1;
    }
    if (enc->dmabuf) {
        snprintf(enc->err, sizeof(enc->err),
                 "this session encodes dma-bufs, not software RGBA frames");
        return -1;
    }
    double t_start = trace_enabled() ? now_ms() : 0.0;
    // A frame of a source that was resized mid-session arrives at the new
    // size and is fitted into the canvas the file was opened with.
    const uint8_t *fitted = fit_rgba_to_canvas(enc, rgba);
    if (fitted == NULL) {
        return -1;
    }
    convert_rgba_to_nv12(enc->sw->data[0], enc->sw->data[1], enc->width, enc->height, fitted);
    double t_convert = trace_enabled() ? now_ms() : 0.0;

    AVFrame *hw = api->frame_alloc();
    if (!hw) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate a hardware frame");
        return -1;
    }
    int ret = api->hwframe_get_buffer(enc->ctx->hw_frames_ctx, hw, 0);
    if (ret < 0) {
        set_err(enc, "could not get a hardware frame from the encoder pool", ret);
        api->frame_free(&hw);
        return -1;
    }
    double t_getbuf = trace_enabled() ? now_ms() : 0.0;
    ret = api->hwframe_transfer_data(hw, enc->sw, 0);
    if (ret < 0) {
        set_err(enc, "uploading the frame to the GPU failed", ret);
        api->frame_free(&hw);
        return -1;
    }
    double t_upload = trace_enabled() ? now_ms() : 0.0;
    hw->pts = enc->frames_sent;
    ret = api->send_frame(enc->ctx, hw);
    api->frame_free(&hw);
    if (ret < 0) {
        set_err(enc, "the encoder rejected a frame", ret);
        return -1;
    }
    double t_send = trace_enabled() ? now_ms() : 0.0;
    enc->frames_sent++;
    int status = drain_packets(enc);
    if (trace_enabled()) {
        double t_drain = now_ms();
        fprintf(stderr,
                "vshot:   shim frame %d: convert %.1f upload %.1f send %.1f drain %.1f ms\n",
                enc->frames_sent, t_convert - t_start, t_upload - t_getbuf,
                t_send - t_upload, t_drain - t_send);
    }
    return status;
}

// Maps one dma-buf to a VAAPI frame, caching by descriptor file descriptor:
// a buffer whose pixels the compositor has just rewritten is simply fed
// through the same mapping again.
static void desc_release(void *opaque, uint8_t *data) {
    (void)opaque;
    free(data);
}

static AVFrame *map_dmabuf(VshotAvEnc *enc, int fd, unsigned fourcc, uint64_t modifier,
                           int offset, int stride) {
    for (int i = 0; i < enc->map_count; i++) {
        if (enc->maps[i].fd == fd) {
            return enc->maps[i].frame;
        }
    }
    if (enc->map_count >= VSHOT_MAX_MAPS) {
        snprintf(enc->err, sizeof(enc->err), "too many dma-buf mappings for one session");
        return NULL;
    }
    AVFrame *drm = api->frame_alloc();
    if (!drm) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate the dma-buf staging frame");
        return NULL;
    }
    AVDRMFrameDescriptor *desc = calloc(1, sizeof(AVDRMFrameDescriptor));
    if (!desc) {
        api->frame_free(&drm);
        snprintf(enc->err, sizeof(enc->err), "out of memory for the dma-buf descriptor");
        return NULL;
    }
    desc->nb_layers = 1;
    desc->nb_objects = 1;
    desc->objects[0].fd = fd;
    desc->objects[0].format_modifier = modifier;
    desc->objects[0].size = (size_t)stride * (size_t)enc->in_h;
    desc->layers[0].format = fourcc;
    desc->layers[0].nb_planes = 1;
    desc->layers[0].planes[0].object_index = 0;
    desc->layers[0].planes[0].offset = offset;
    desc->layers[0].planes[0].pitch = stride;
    drm->width = enc->in_w;
    drm->height = enc->in_h;
    drm->format = AV_PIX_FMT_DRM_PRIME;
    drm->data[0] = (uint8_t *)desc;
    // The descriptor is plain C memory and the buffer owns it: `free` is
    // the release callback.  (av_buffer_create wants a non-NULL callback
    // when the data is not reference-counted.)
    drm->buf[0] = api->buffer_create((uint8_t *)desc, sizeof(*desc),
                                     desc_release, NULL, 0);
    if (!drm->buf[0]) {
        free(desc);
        api->frame_free(&drm);
        snprintf(enc->err, sizeof(enc->err), "could not wrap the dma-buf descriptor");
        return NULL;
    }
    AVFrame *mapped = api->frame_alloc();
    if (!mapped) {
        api->frame_unref(drm);
        api->frame_free(&drm);
        snprintf(enc->err, sizeof(enc->err), "could not allocate the VAAPI mapping frame");
        return NULL;
    }
    mapped->format = AV_PIX_FMT_VAAPI;
    mapped->hw_frames_ctx = api->buffer_ref(enc->frames);
    int ret = api->hwframe_map(mapped, drm, AV_HWFRAME_MAP_READ);
    api->frame_unref(drm);
    api->frame_free(&drm);
    if (ret < 0) {
        api->frame_free(&mapped);
        set_err(enc, "could not map the dma-buf to a VAAPI surface", ret);
        return NULL;
    }
    enc->maps[enc->map_count].fd = fd;
    enc->maps[enc->map_count].frame = mapped;
    enc->map_count++;
    return mapped;
}

// Sends one dma-buf frame: maps it, runs it through the filtergraph (the
// GPU converts to NV12) and hands the result to the encoder.
int vshot_av_enc_send_dmabuf(VshotAvEnc *enc, int fd, unsigned fourcc, uint64_t modifier,
                             int offset, int stride) {
    enc->out_len = 0;
    if (!api || !enc || !enc->ctx) {
        return -1;
    }
    if (!enc->dmabuf) {
        snprintf(enc->err, sizeof(enc->err),
                 "this session encodes software frames, not dma-bufs");
        return -1;
    }
    if (fourcc != enc->fourcc) {
        snprintf(enc->err, sizeof(enc->err),
                 "the dma-buf format changed mid-recording (0x%08x, expected 0x%08x)", fourcc,
                 enc->fourcc);
        return -1;
    }
    double t_start = trace_enabled() ? now_ms() : 0.0;
    AVFrame *mapped = map_dmabuf(enc, fd, fourcc, modifier, offset, stride);
    if (!mapped) {
        return -1;
    }
    double t_map = trace_enabled() ? now_ms() : 0.0;
    // The cached mapping is reused for every frame that buffer carries, so
    // the filtergraph gets a reference to it, not the frame itself: the
    // buffersrc takes ownership of whatever it is handed.
    AVFrame *feed = api->frame_alloc();
    if (!feed) {
        snprintf(enc->err, sizeof(enc->err), "could not allocate a feed frame");
        return -1;
    }
    int ret = api->frame_ref(feed, mapped);
    if (ret < 0) {
        api->frame_free(&feed);
        set_err(enc, "could not reference the mapped frame", ret);
        return -1;
    }
    feed->pts = enc->frames_sent;
    ret = api->buffersrc_add_frame_flags(enc->graph_src, feed, 0);
    if (ret < 0) {
        api->frame_free(&feed);
        set_err(enc, "feeding the filtergraph failed", ret);
        return -1;
    }
    api->frame_free(&feed);
    // A fit graph has a second input: the black plate its letterbox is
    // composed over.  It needs one reference per output frame — the
    // framesync behind overlay_vaapi pairs the two by frame — and the same
    // uploaded plate serves all of them.
    if (enc->graph_plate && enc->plate) {
        AVFrame *plate = api->frame_alloc();
        if (!plate) {
            snprintf(enc->err, sizeof(enc->err), "could not allocate a plate reference");
            return -1;
        }
        ret = api->frame_ref(plate, enc->plate);
        if (ret >= 0) {
            plate->pts = enc->frames_sent;
            ret = api->buffersrc_add_frame_flags(enc->graph_plate, plate, 0);
        }
        api->frame_free(&plate);
        if (ret < 0) {
            set_err(enc, "feeding the plate to the filtergraph failed", ret);
            return -1;
        }
    }
    double t_filter_in = trace_enabled() ? now_ms() : 0.0;
    // The conversion may lag the input by nothing at all (no buffering in
    // scale_vaapi), but the contract is to drain whatever the graph emits.
    for (;;) {
        AVFrame *filtered = api->frame_alloc();
        if (!filtered) {
            snprintf(enc->err, sizeof(enc->err), "could not allocate a filtered frame");
            return -1;
        }
        ret = api->buffersink_get_frame(enc->graph_sink, filtered);
        if (ret == AVERROR(EAGAIN)) {
            api->frame_free(&filtered);
            break;
        }
        if (ret < 0) {
            api->frame_free(&filtered);
            set_err(enc, "pulling from the filtergraph failed", ret);
            return -1;
        }
        ret = api->send_frame(enc->ctx, filtered);
        api->frame_free(&filtered);
        if (ret < 0) {
            set_err(enc, "the encoder rejected a filtered frame", ret);
            return -1;
        }
    }
    double t_filter = trace_enabled() ? now_ms() : 0.0;
    enc->frames_sent++;
    int status = drain_packets(enc);
    if (trace_enabled()) {
        double t_drain = now_ms();
        fprintf(stderr,
                "vshot:   shim frame %d (dmabuf): map %.1f add %.1f filter %.1f drain %.1f ms\n",
                enc->frames_sent, t_map - t_start, t_filter_in - t_map, t_filter - t_filter_in,
                t_drain - t_filter);
    }
    return status;
}

// Rebuilds the zero-copy chain for a new input size, fitting the frames into
// the encoder's canvas.
//
// `record window` records one window's own pixels, and a window can be
// resized while the recording runs.  The compositor re-sends its buffer
// constraints at the new size (and refuses frames of the old one), and the
// capture pool is rebuilt — but the encoder was opened once for the
// recording's canvas, and one MP4 holds one frame size.  So the input side
// is what moves: the old filtergraph (and the old input pool it mapped
// through) is torn down, a new one is built for the new size, and it scales
// the frames down until they fit the canvas and letterboxes them into it.
// A frame's geometry on screen is preserved, and the file keeps the same
// dimensions from its first packet to its trailer.
//
// Freeing the old graph while the encoder still has a couple of pictures in
// flight is safe: the encoder took its own references to the filtered frames
// it was handed (measured — see the `fit-midstream-probe`).
int vshot_av_enc_resize_fit(VshotAvEnc *enc, int in_w, int in_h, unsigned fourcc) {
    if (!api || !enc || !enc->ctx) {
        return -1;
    }
    if (in_w <= 0 || in_h <= 0) {
        snprintf(enc->err, sizeof(enc->err), "refusing a %dx%d resize", in_w, in_h);
        return -1;
    }
    if (!enc->dmabuf) {
        // The software path has no filtergraph to rebuild: the fit is done on
        // the CPU, in `vshot_av_enc_send`, and all this has to do is record
        // the new input size the frames will arrive at.  The encoder's canvas
        // — and so the file's shape — stays what it was opened for, exactly
        // as on the dma-buf path.
        enc->in_w = in_w;
        enc->in_h = in_h;
        return 0;
    }
    // A rebuild is also how a compositor that changes its offered format
    // across a resize is followed: the new fourcc becomes the session's.
    if (fourcc == 0) {
        fourcc = enc->fourcc;
    }
    if (in_w == enc->in_w && in_h == enc->in_h && fourcc == enc->fourcc) {
        return 0;
    }
    // The old chain goes first: the graph holds the input pool referenced by
    // the mappings, and both are replaced together.  The plate and its pool
    // belong to the chain too — the rebuilt graph builds its own.
    for (int i = 0; i < enc->map_count; i++) {
        if (enc->maps[i].frame) {
            api->frame_unref(enc->maps[i].frame);
            api->frame_free(&enc->maps[i].frame);
        }
    }
    enc->map_count = 0;
    teardown_graph(enc);
    int sw_format = fourcc_to_sw_format(fourcc);
    if (sw_format < 0) {
        snprintf(enc->err, sizeof(enc->err),
                 "the dma-buf format 0x%08x is not a packed RGB format the encoder accepts",
                 fourcc);
        return -1;
    }
    // How the new graph is built depends on which side is bigger: an input
    // larger than the canvas is scaled down to fit; a smaller one is left at
    // its own size and centred — the fit chain expresses exactly that.
    if (open_filtergraph_dmabuf_fit(enc, sw_format, in_w, in_h, enc->width, enc->height) != 0) {
        return -1;
    }
    // The sink pool must still match what the encoder was opened with: same
    // size, same format.  A mismatch would be a bug in the fit chain rather
    // than a caller's mistake, so it is checked rather than assumed.
    AVFilterLink *link = enc->graph_sink->inputs[0];
    if (link->w != enc->width || link->h != enc->height) {
        snprintf(enc->err, sizeof(enc->err),
                 "the rebuilt fit chain is %dx%d, but the recording is %dx%d", link->w, link->h,
                 enc->width, enc->height);
        return -1;
    }
    enc->in_w = in_w;
    enc->in_h = in_h;
    enc->fourcc = fourcc;
    if (trace_enabled()) {
        fprintf(stderr, "vshot:   shim fit chain rebuilt for %dx%d (fourcc 0x%08x) into %dx%d\n",
                in_w, in_h, fourcc, enc->width, enc->height);
    }
    return 0;
}

// Flushes the encoder: the frames still in flight come out of `take`.
int vshot_av_enc_flush(VshotAvEnc *enc) {
    enc->out_len = 0;
    if (!api || !enc || !enc->ctx) {
        return -1;
    }
    // A recording whose source never produced a frame arrives here with an
    // encoder that has seen nothing: the VAAPI encoders' drain path assumes
    // at least one picture was issued and a flush of an empty stream
    // crashes inside libavcodec (measured: a static window recording killed
    // with SIGTERM segfaulted in avcodec_send_frame before this guard).
    // There is nothing to flush, and the muxer still writes its header and
    // trailer — a playable zero-frame file rather than a dead process.
    if (enc->frames_sent == 0) {
        return 0;
    }
    if (enc->dmabuf && enc->graph) {
        // The filtergraph has to be flushed first: pushing NULL through it
        // is what sends its own tail to the sink.  A fit graph has two
        // inputs — content and plate — and both are ended, or the framesync
        // behind overlay_vaapi would wait on the one still open.
        int ret = api->buffersrc_add_frame_flags(enc->graph_src, NULL, 0);
        if (ret < 0 && ret != AVERROR_EOF) {
            set_err(enc, "flushing the filtergraph failed", ret);
            return -1;
        }
        if (enc->graph_plate) {
            ret = api->buffersrc_add_frame_flags(enc->graph_plate, NULL, 0);
            if (ret < 0 && ret != AVERROR_EOF) {
                set_err(enc, "flushing the plate's stream failed", ret);
                return -1;
            }
        }
        for (;;) {
            AVFrame *filtered = api->frame_alloc();
            if (!filtered) {
                snprintf(enc->err, sizeof(enc->err), "could not allocate a filtered frame");
                return -1;
            }
            ret = api->buffersink_get_frame(enc->graph_sink, filtered);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                api->frame_free(&filtered);
                break;
            }
            if (ret < 0) {
                api->frame_free(&filtered);
                set_err(enc, "pulling from the filtergraph while flushing failed", ret);
                return -1;
            }
            ret = api->send_frame(enc->ctx, filtered);
            api->frame_free(&filtered);
            if (ret < 0 && ret != AVERROR_EOF) {
                set_err(enc, "the encoder rejected a tail frame", ret);
                return -1;
            }
        }
    }
    int ret = api->send_frame(enc->ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF) {
        set_err(enc, "flushing the encoder failed", ret);
        return -1;
    }
    return drain_packets(enc);
}

// The bytes accumulated by the last send/flush.  The pointer stays valid
// until the next send, flush or destroy; the caller copies what it needs.
const uint8_t *vshot_av_enc_take(VshotAvEnc *enc, int *len) {
    if (!enc) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    if (len) {
        *len = (int)enc->out_len;
    }
    return enc->out;
}

// The encoder's extradata: the parameter sets libavcodec produced at open
// (with AV_CODEC_FLAG_GLOBAL_HEADER).  The muxer builds the MP4 sample
// entry's configuration record from these, the way libavformat does.
const uint8_t *vshot_av_enc_extradata(VshotAvEnc *enc, int *len) {
    if (!enc || !enc->ctx) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    if (len) {
        *len = enc->ctx->extradata_size;
    }
    return enc->ctx->extradata;
}

const char *vshot_av_enc_last_error(VshotAvEnc *enc) {
    if (!enc) {
        return api_error[0] ? api_error : "libavcodec is unavailable";
    }
    return enc->err;
}

// A probe the Rust side uses to report which ffmpeg it found, for
// `VSHOT_RECORD_DEBUG` and the tests.
int vshot_av_enc_available(void) { return load_api() != NULL; }
int vshot_av_enc_filter_available(void) {
    api = load_api();
    return api && api->filter_loaded;
}
const char *vshot_av_enc_load_error(void) {
    if (api_error[0]) {
        return api_error;
    }
    return create_error;
}
unsigned vshot_av_enc_version(void) {
    api = load_api();
    return api ? api->codec_version() : 0;
}

// ---------------------------------------------------------------------------
// The recorder: one encoder, plus the MP4 muxer libavformat provides
// ---------------------------------------------------------------------------
//
// This is wf-recorder's shape — the encoder's packets go straight into
// ffmpeg's own mp4 muxer, so the container is written by the same library
// that produced the bitstream.  Doing it by hand meant rebuilding avcC /
// hvcC / av1C and the sample tables out of an Annex-B byte stream, and that
// reconstruction is exactly where container and bitstream drifted apart:
// the parameter sets an encoder reports at open time are not always the
// ones it codes against (measured here: an open-time PPS ending `38 b0`
// over slices coded against `38 30`), and AV1 needed its OBU stream put
// back together.  libavformat takes the packet as it is.

/* ---------------------------------------------------------------------------
 * The microphone encoder
 * ---------------------------------------------------------------------------
 *
 * `record --mic` records a soundtrack beside the video: the microphone's
 * samples are encoded with AAC (ffmpeg's own encoder, in the same libavcodec
 * this file already loads) and muxed as a second stream in the same MP4.  The
 * video side is untouched — same encoder, same muxer — and the audio stream
 * is created before the header is written, because a container's streams are
 * declared in its header.
 *
 * The audio encoder is opened with the rate and channel count the
 * microphone's PipeWire negotiation settled on, which is why the microphone
 * client is opened before the recorder.  A sample rate the AAC encoder does
 * not take is resampled to 48 kHz inside the encoder.
 *
 * The same encoder also carries an application's own playback (`--app-audio`):
 * the client is a PipeWire capture stream bound to that application's output
 * node, and the samples arrive here exactly as a microphone's do.  Nothing
 * below distinguishes the two — one capture source, one AAC stream.
 */

// The sample rates the AAC encoder takes, best first.
static const int vshot_audio_rates[] = {48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000};

typedef struct VshotAudioEnc {
    AVCodecContext *ctx;
    AVFrame *frame;      // the planar-float staging frame
    float *pending;      // interleaved float samples not yet in a whole frame
    int pending_samples; // in frames (samples per channel)
    int pending_cap;     // frames the staging buffer can hold
    int64_t next_pts;    // in samples, the encoder's own timeline
    int rate;
    int channels;
    char err[256];
} VshotAudioEnc;

// The rate the AAC encoder takes: the microphone's own when it is one of
// them, else 48 kHz (the encoder's resampler handles the rest).
static int vshot_audio_pick_rate(int device_rate) {
    for (size_t i = 0; i < sizeof(vshot_audio_rates) / sizeof(vshot_audio_rates[0]); i++) {
        if (vshot_audio_rates[i] == device_rate) {
            return device_rate;
        }
    }
    return 48000;
}

// Creates and opens the AAC encoder.  Returns NULL with `err` set.
static VshotAudioEnc *vshot_audio_enc_create(int device_rate, int channels, char *err,
                                             size_t err_len) {
    api = load_api();
    if (!api) {
        snprintf(err, err_len, "%s", api_error);
        return NULL;
    }
    if (channels < 1 || channels > 2) {
        snprintf(err, err_len,
                 "the microphone reports %d channels; one or two are supported", channels);
        return NULL;
    }
    const AVCodec *codec = api->find_encoder_by_name("aac");
    if (!codec) {
        snprintf(err, err_len, "this ffmpeg build has no aac encoder");
        return NULL;
    }
    VshotAudioEnc *audio = calloc(1, sizeof(*audio));
    if (!audio) {
        snprintf(err, err_len, "out of memory for the audio encoder");
        return NULL;
    }
    audio->rate = vshot_audio_pick_rate(device_rate);
    audio->channels = channels;
    audio->ctx = api->alloc_context3(codec);
    if (!audio->ctx) {
        snprintf(err, err_len, "could not allocate the audio encoder context");
        free(audio);
        return NULL;
    }
    audio->ctx->sample_rate = audio->rate;
    audio->ctx->time_base = (AVRational){1, audio->rate};
    if (api->channel_layout_default) {
        api->channel_layout_default(&audio->ctx->ch_layout, channels);
    }
    audio->ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    audio->ctx->bit_rate = channels == 1 ? 96000 : 160000;
    audio->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int ret = api->open2(audio->ctx, codec, NULL);
    if (ret < 0) {
        char detail[AV_ERROR_MAX_STRING_SIZE] = {0};
        api->strerror(ret, detail, sizeof(detail));
        snprintf(err, err_len, "opening the aac encoder failed: %s (%d)", detail, ret);
        api->free_context(&audio->ctx);
        free(audio);
        return NULL;
    }
    audio->frame = api->frame_alloc();
    if (!audio->frame) {
        snprintf(err, err_len, "could not allocate the audio frame");
        api->free_context(&audio->ctx);
        free(audio);
        return NULL;
    }
    return audio;
}

static void vshot_audio_enc_destroy(VshotAudioEnc *audio) {
    if (!audio) {
        return;
    }
    free(audio->pending);
    if (audio->frame && api) {
        api->frame_free(&audio->frame);
    }
    if (audio->ctx && api) {
        api->free_context(&audio->ctx);
    }
    free(audio);
}

// Queues interleaved float samples for encoding.  The staging buffer grows
// on demand; a frame's worth of audio is a few kilobytes, so it is allocated
// once and reused.
static int vshot_audio_enc_feed(VshotAudioEnc *audio, const float *samples, int frames) {
    if (frames <= 0) {
        return 0;
    }
    int needed = audio->pending_samples + frames;
    if (needed > audio->pending_cap) {
        int cap = audio->pending_cap ? audio->pending_cap : 4096;
        while (cap < needed) {
            cap *= 2;
        }
        float *grown = realloc(audio->pending, (size_t)cap * audio->channels * sizeof(float));
        if (!grown) {
            snprintf(audio->err, sizeof(audio->err),
                     "out of memory for the audio staging buffer");
            return -1;
        }
        audio->pending = grown;
        audio->pending_cap = cap;
    }
    memcpy(audio->pending + (size_t)audio->pending_samples * audio->channels, samples,
           (size_t)frames * audio->channels * sizeof(float));
    audio->pending_samples += frames;
    return 0;
}

typedef struct VshotRec {
    VshotAvEnc *enc;
    AVFormatContext *fmt;
    AVStream *stream;
    // The microphone's soundtrack, when `record --mic` asked for one.  The
    // audio stream is created before the header is written (a container's
    // streams are declared in its header), and the encoder feeding it lives
    // here because the two are the same recording's.
    VshotAudioEnc *audio;
    AVStream *audio_stream;
    AVRational audio_time_base; // the muxer's, read back after the header
    int finished;
    char err[256];
} VshotRec;

void vshot_rec_free(VshotRec *rec);

// Hands one encoded audio packet to the recorder's audio stream.  The
// encoder's own sample timeline is rescaled into the muxer's time base, the
// same conversion the video packets go through.
static int vshot_audio_write_packet(VshotAudioEnc *audio, VshotRec *rec, AVPacket *pkt) {
    if (!rec) {
        return 0;
    }
    // A replay keeps its audio in the ring too, on the same millisecond
    // timeline the video packets use, so a save can interleave the two by
    // time.  The encoder's timeline is samples; the rate turns it into ms.
    if (rec->enc && rec->enc->replay) {
        int64_t ms = audio->rate > 0 ? pkt->pts * 1000 / audio->rate : 0;
        int64_t dur = audio->rate > 0 ? pkt->duration * 1000 / audio->rate : 0;
        int is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if (replay_push(rec->enc->replay, pkt, 1, is_key, ms, dur) != 0) {
            snprintf(audio->err, sizeof(audio->err),
                     "keeping an audio packet in the replay ring failed");
            return -1;
        }
        return 0;
    }
    if (!rec->audio_stream || !rec->fmt) {
        return 0;
    }
    AVRational sample = {1, audio->rate};
    pkt->stream_index = rec->audio_stream->index;
    pkt->pts = pkt->dts = api->rescale_q(pkt->pts, sample, rec->audio_time_base);
    pkt->duration = api->rescale_q(pkt->duration, sample, rec->audio_time_base);
    int written = api->format_write_frame(rec->fmt, pkt);
    if (written < 0) {
        char detail[AV_ERROR_MAX_STRING_SIZE] = {0};
        api->strerror(written, detail, sizeof(detail));
        snprintf(audio->err, sizeof(audio->err),
                 "writing an audio packet to the MP4 muxer failed: %s (%d)", detail, written);
        return -1;
    }
    return 0;
}

// Drains every packet the encoder has ready.
static int vshot_audio_drain(VshotAudioEnc *audio, VshotRec *rec) {
    for (;;) {
        AVPacket *pkt = api->packet_alloc();
        if (!pkt) {
            snprintf(audio->err, sizeof(audio->err), "could not allocate an audio packet");
            return -1;
        }
        int ret = api->receive_packet(audio->ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            api->packet_free(&pkt);
            break;
        }
        if (ret < 0) {
            char detail[AV_ERROR_MAX_STRING_SIZE] = {0};
            api->strerror(ret, detail, sizeof(detail));
            api->packet_free(&pkt);
            snprintf(audio->err, sizeof(audio->err),
                     "receiving an encoded audio packet failed: %s (%d)", detail, ret);
            return -1;
        }
        int status = vshot_audio_write_packet(audio, rec, pkt);
        api->packet_free(&pkt);
        if (status != 0) {
            return -1;
        }
    }
    return 0;
}

// Encodes every whole frame the staging buffer holds.  `flush` also sends the
// tail, padded with silence to a whole frame: the encoder needs whole frames,
// and the last partial frame of a recording is silence by definition.
static int vshot_audio_enc_pump(VshotAudioEnc *audio, VshotRec *rec, int flush) {
    int frame_size = audio->ctx->frame_size > 0 ? audio->ctx->frame_size : 1024;

    for (;;) {
        int have = audio->pending_samples;
        int take = have < frame_size ? have : frame_size;
        if (take < frame_size && !flush) {
            break;
        }
        // `av_frame_get_buffer` allocates from the frame's own description:
        // the format, the sample count and the channel layout must be set
        // *before* the call, or it cannot size the planes (which it answers
        // with a bare refusal otherwise).
        audio->frame->format = AV_SAMPLE_FMT_FLTP;
        audio->frame->sample_rate = audio->rate;
        audio->frame->nb_samples = frame_size;
        if (api->channel_layout_default) {
            api->channel_layout_default(&audio->frame->ch_layout, audio->channels);
        }
        int ret = api->frame_get_buffer(audio->frame, 0);
        if (ret < 0) {
            char detail[AV_ERROR_MAX_STRING_SIZE] = {0};
            api->strerror(ret, detail, sizeof(detail));
            snprintf(audio->err, sizeof(audio->err),
                     "could not get an audio frame buffer: %s (%d)", detail, ret);
            return -1;
        }
        audio->frame->pts = audio->next_pts;
        // Planar float: channel c's samples live in data[c].
        for (int c = 0; c < audio->channels; c++) {
            float *plane = (float *)audio->frame->data[c];
            for (int i = 0; i < frame_size; i++) {
                plane[i] = i < take ? audio->pending[(size_t)i * audio->channels + c] : 0.0f;
            }
        }
        if (take > 0) {
            memmove(audio->pending, audio->pending + (size_t)take * audio->channels,
                    (size_t)(have - take) * audio->channels * sizeof(float));
            audio->pending_samples = have - take;
        }
        audio->next_pts += frame_size;
        ret = api->send_frame(audio->ctx, audio->frame);
        if (ret < 0) {
            char detail[AV_ERROR_MAX_STRING_SIZE] = {0};
            api->strerror(ret, detail, sizeof(detail));
            snprintf(audio->err, sizeof(audio->err),
                     "the aac encoder rejected a frame: %s (%d)", detail, ret);
            return -1;
        }
        if (vshot_audio_drain(audio, rec) != 0) {
            return -1;
        }
        if (take < frame_size && flush) {
            // The padded tail was the last frame.
            break;
        }
        if (!flush && audio->pending_samples == 0) {
            break;
        }
    }
    return 0;
}

// Flushes the encoder: the samples still inside it come out as packets.  The
// muxer writes them before its trailer, so the audio track ends where the
// samples did.
static int vshot_audio_enc_flush(VshotAudioEnc *audio, VshotRec *rec) {
    if (!audio || !audio->ctx) {
        return 0;
    }
    if (vshot_audio_enc_pump(audio, rec, 1) != 0) {
        return -1;
    }
    int ret = api->send_frame(audio->ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF) {
        char detail[AV_ERROR_MAX_STRING_SIZE] = {0};
        api->strerror(ret, detail, sizeof(detail));
        snprintf(audio->err, sizeof(audio->err), "flushing the aac encoder failed: %s (%d)",
                 detail, ret);
        return -1;
    }
    return vshot_audio_drain(audio, rec);
}

// ---------------------------------------------------------------------------
// The recorder: one encoder, plus the MP4 muxer libavformat provides
// ---------------------------------------------------------------------------
// AV_CODEC_ID_* for the short names `--encoder` accepts.
static enum AVCodecID codec_id_for_word(const char *codec) {
    if (strcmp(codec, "h264") == 0) {
        return AV_CODEC_ID_H264;
    }
    if (strcmp(codec, "hevc") == 0) {
        return AV_CODEC_ID_HEVC;
    }
    if (strcmp(codec, "av1") == 0) {
        return AV_CODEC_ID_AV1;
    }
    return AV_CODEC_ID_NONE;
}

// Copies the encoder's message up, or keeps the one already there.
static void rec_take_enc_error(VshotRec *rec, const char *fallback) {
    if (rec->enc && rec->enc->err[0]) {
        snprintf(rec->err, sizeof(rec->err), "%s", rec->enc->err);
    } else {
        snprintf(rec->err, sizeof(rec->err), "%s", fallback);
    }
}

// Wraps an already-open encoder in an MP4 container.  The stream learns the
// codec's parameters through avcodec_parameters_from_context, the call
// wf-recorder makes too; anything the encoder repeats in-band it prefers on
// its own, so the two never disagree in the file.
static int rec_open_muxer(VshotRec *rec, const char *path, int width, int height,
                          const char *codec) {
    if (!api->format_loaded) {
        snprintf(rec->err, sizeof(rec->err),
                 "libavformat is not installed, so MP4 recording is unavailable "
                 "(install ffmpeg)");
        return -1;
    }
    enum AVCodecID id = codec_id_for_word(codec);
    if (id == AV_CODEC_ID_NONE) {
        snprintf(rec->err, sizeof(rec->err), "the codec %s has no MP4 sample entry", codec);
        return -1;
    }
    int ret = api->format_alloc_output(&rec->fmt, NULL, "mp4", path);
    if (ret < 0 || !rec->fmt) {
        snprintf(rec->err, sizeof(rec->err), "could not open an MP4 muxer for %s", path);
        return -1;
    }
    rec->stream = api->format_new_stream(rec->fmt, NULL);
    if (!rec->stream) {
        snprintf(rec->err, sizeof(rec->err), "could not add a video stream to the MP4 muxer");
        return -1;
    }
    // Milliseconds: every frame carries the time it really spent on screen,
    // so the timeline is variable and states that rather than faking a
    // nominal rate.  This is the unit the encoder side speaks; the muxer
    // gets to answer with its own time base below.
    rec->stream->time_base = (AVRational){1, 1000};
    ret = api->parameters_from_context(rec->stream->codecpar, rec->enc->ctx);
    if (ret < 0) {
        set_err(rec->enc, "the MP4 muxer refused the codec parameters", ret);
        rec_take_enc_error(rec, "the MP4 muxer refused the codec parameters");
        return -1;
    }
    rec->stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    rec->stream->codecpar->width = width;
    rec->stream->codecpar->height = height;
    // The soundtrack, when there is one.  Its stream has to exist before the
    // header is written: a container's streams are declared in its header,
    // and movenc writes the track list there.
    if (rec->audio) {
        rec->audio_stream = api->format_new_stream(rec->fmt, NULL);
        if (!rec->audio_stream) {
            snprintf(rec->err, sizeof(rec->err),
                     "could not add an audio stream to the MP4 muxer");
            return -1;
        }
        rec->audio_stream->time_base = (AVRational){1, rec->audio->rate};
        ret = api->parameters_from_context(rec->audio_stream->codecpar, rec->audio->ctx);
        if (ret < 0) {
            snprintf(rec->err, sizeof(rec->err),
                     "the MP4 muxer refused the audio codec parameters (%d)", ret);
            return -1;
        }
        rec->audio_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        rec->audio_stream->codecpar->sample_rate = rec->audio->rate;
    }
    if (!(rec->fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = api->io_open(&rec->fmt->pb, path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            set_err(rec->enc, "could not open the output file for writing", ret);
            rec_take_enc_error(rec, "could not open the output file for writing");
            return -1;
        }
    }
    ret = api->format_write_header(rec->fmt, NULL);
    if (ret < 0) {
        set_err(rec->enc, "writing the MP4 header failed", ret);
        rec_take_enc_error(rec, "writing the MP4 header failed");
        return -1;
    }
    // Past the header the muxer owns the stream's time base: what was set
    // above is a request, and movenc answers it with its own (1/16000 for
    // the millisecond base).  Packet timestamps are read in this unit, so it
    // is kept and every packet is rescaled into it.
    rec->enc->mux_time_base = rec->stream->time_base;
    if (rec->audio_stream) {
        rec->audio_time_base = rec->audio_stream->time_base;
    }
    if (trace_enabled()) {
        fprintf(stderr, "vshot: MP4 stream time base %d/%d\n",
                rec->stream->time_base.num, rec->stream->time_base.den);
        if (rec->audio_stream) {
            fprintf(stderr, "vshot: MP4 audio time base %d/%d (%d Hz, %d channels)\n",
                    rec->audio_stream->time_base.num, rec->audio_stream->time_base.den,
                    rec->audio->rate, rec->audio->channels);
        }
    }
    rec->enc->mux = rec->fmt;
    rec->enc->mux_stream = rec->stream;
    rec->enc->mux_next_pts = 0;
    rec->enc->mux_frames = 0;
    return 0;
}

static VshotRec *rec_start(const char *path, int width, int height, const char *codec, int qp,
                           int dmabuf, unsigned fourcc, int mic_rate, int mic_channels,
                           int backend) {
    api = load_api();
    if (!api) {
        return NULL;
    }
    if (!path || !path[0] || !codec || !codec[0] || width <= 0 || height <= 0) {
        snprintf(create_error, sizeof(create_error), "the recorder was given an empty shape");
        return NULL;
    }
    // NVENC cannot import a compositor dma-buf, so a request that pairs the two
    // is recorded through the software path instead — the capture side hands
    // over RGBA pixels, which the shim converts and uploads.  The Rust side
    // makes the same decision before it opens the capture pool; this is the
    // belt to that suspenders, because a zero-copy request that reached here
    // would otherwise fail at the filtergraph.
    if (dmabuf && backend == VSHOT_BACKEND_NVENC) {
        dmabuf = 0;
        fourcc = 0;
    }
    VshotRec *rec = calloc(1, sizeof(VshotRec));
    if (!rec) {
        snprintf(create_error, sizeof(create_error), "out of memory for the recorder");
        return NULL;
    }
    // The microphone's encoder is opened first: its rate and channel count
    // are the ones the muxer's audio stream declares, and that stream has to
    // exist before the header is written.
    if (mic_rate > 0 && mic_channels > 0) {
        char detail[256] = {0};
        rec->audio = vshot_audio_enc_create(mic_rate, mic_channels, detail, sizeof(detail));
        if (!rec->audio) {
            snprintf(create_error, sizeof(create_error), "%s", detail);
            free(rec);
            return NULL;
        }
    }
    rec->enc = dmabuf ? vshot_av_enc_create_dmabuf(width, height, codec, qp, 0, fourcc, backend)
                      : vshot_av_enc_create(width, height, codec, qp, 0, backend);
    if (!rec->enc) {
        // `vshot_av_enc_load_error` reads the same module-level buffer this
        // would write, so copy through a local first.
        char detail[sizeof(create_error)];
        snprintf(detail, sizeof(detail), "%s", vshot_av_enc_load_error());
        snprintf(create_error, sizeof(create_error), "%s", detail);
        vshot_rec_free(rec);
        return NULL;
    }
    if (rec_open_muxer(rec, path, width, height, codec) != 0) {
        snprintf(create_error, sizeof(create_error), "%s", rec->err);
        vshot_rec_free(rec);
        return NULL;
    }
    return rec;
}

// Starts a recording: an encoder for `codec` plus the MP4 file at `path`.
// `backend` is VSHOT_BACKEND_VAAPI (0) or VSHOT_BACKEND_NVENC (1).  Returns
// NULL with the reason in `vshot_rec_load_error`.
VshotRec *vshot_rec_start(const char *path, int width, int height, const char *codec, int qp,
                          int backend) {
    return rec_start(path, width, height, codec, qp, 0, 0, 0, 0, backend);
}

// The same, with a microphone: a soundtrack is recorded beside the video
// when both the rate and the channel count are positive.
VshotRec *vshot_rec_start_mic(const char *path, int width, int height, const char *codec, int qp,
                              int mic_rate, int mic_channels, int backend) {
    return rec_start(path, width, height, codec, qp, 0, 0, mic_rate, mic_channels, backend);
}

// The zero-copy variant: the frames are dma-bufs with this fourcc.
VshotRec *vshot_rec_start_dmabuf(const char *path, int width, int height, const char *codec,
                                 int qp, unsigned fourcc, int backend) {
    return rec_start(path, width, height, codec, qp, 1, fourcc, 0, 0, backend);
}

// The zero-copy variant with a microphone.
VshotRec *vshot_rec_start_dmabuf_mic(const char *path, int width, int height, const char *codec,
                                     int qp, unsigned fourcc, int mic_rate, int mic_channels,
                                     int backend) {
    return rec_start(path, width, height, codec, qp, 1, fourcc, mic_rate, mic_channels, backend);
}

// Feeds one RGBA frame and hands its packets to the muxer.  `duration_ms` is
// how long the frame was on screen.
int vshot_rec_frame(VshotRec *rec, const uint8_t *rgba, int duration_ms) {
    if (!rec || !rec->enc) {
        return -1;
    }
    if (rec->finished) {
        snprintf(rec->err, sizeof(rec->err), "the recorder was already finished");
        return -1;
    }
    // The duration is queued before the frame goes in, and taken back out
    // when that frame's packet comes out at the other end.
    push_duration(rec->enc, duration_ms);
    int status = vshot_av_enc_send(rec->enc, rgba);
    if (status != 0) {
        rec_take_enc_error(rec, "encoding a frame failed");
    }
    return status;
}

// The zero-copy variant of `vshot_rec_frame`.
int vshot_rec_frame_dmabuf(VshotRec *rec, int fd, unsigned fourcc, uint64_t modifier,
                           int offset, int stride, int duration_ms) {
    if (!rec || !rec->enc) {
        return -1;
    }
    if (rec->finished) {
        snprintf(rec->err, sizeof(rec->err), "the recorder was already finished");
        return -1;
    }
    push_duration(rec->enc, duration_ms);
    int status =
        vshot_av_enc_send_dmabuf(rec->enc, fd, fourcc, modifier, offset, stride);
    if (status != 0) {
        rec_take_enc_error(rec, "encoding a dma-buf frame failed");
    }
    return status;
}

// The source was resized mid-recording: the zero-copy chain is rebuilt so
// the new-size frames are fitted into the canvas the file was opened with.
// No duration is queued — this produces no packet, it only changes what the
// next `vshot_rec_frame_dmabuf` feeds the encoder.
int vshot_rec_resize_fit(VshotRec *rec, int width, int height, unsigned fourcc) {
    if (!rec || !rec->enc) {
        return -1;
    }
    if (rec->finished) {
        snprintf(rec->err, sizeof(rec->err), "the recorder was already finished");
        return -1;
    }
    if (vshot_av_enc_resize_fit(rec->enc, width, height, fourcc) != 0) {
        rec_take_enc_error(rec, "resizing the capture chain failed");
        return -1;
    }
    return 0;
}

// Flushes the encoder's remaining frames into the muxer and writes the
// trailer, leaving a seekable file behind.
int vshot_rec_finish(VshotRec *rec) {
    if (!rec || !rec->enc) {
        return -1;
    }
    if (rec->finished) {
        return 0;
    }
    rec->finished = 1;
    // The frames the encoder was still holding come out now, each with the
    // duration it queued on the way in, so the file ends where the wall clock
    // did.
    if (vshot_av_enc_flush(rec->enc) != 0) {
        rec_take_enc_error(rec, "flushing the encoder failed");
        return -1;
    }
    // The microphone's tail goes in before the trailer: the samples still
    // inside the AAC encoder become packets, and the trailer then indexes
    // both tracks.
    if (rec->audio) {
        if (vshot_audio_enc_flush(rec->audio, rec) != 0) {
            snprintf(rec->err, sizeof(rec->err), "%s", rec->audio->err);
            return -1;
        }
    }
    if (rec->fmt) {
        int ret = api->format_write_trailer(rec->fmt);
        if (ret < 0) {
            set_err(rec->enc, "writing the MP4 trailer failed", ret);
            rec_take_enc_error(rec, "writing the MP4 trailer failed");
            return -1;
        }
        if (rec->fmt->pb) {
            api->io_closep(&rec->fmt->pb);
        }
    }
    return 0;
}

// How many packets reached the muxer, and how long the timeline is.
int64_t vshot_rec_frames(VshotRec *rec) { return rec && rec->enc ? rec->enc->mux_frames : 0; }

double vshot_rec_seconds(VshotRec *rec) {
    return rec && rec->enc ? (double)rec->enc->mux_next_pts / 1000.0 : 0.0;
}

const char *vshot_rec_last_error(VshotRec *rec) {
    if (!rec) {
        return api_error[0] ? api_error : "the recorder is unavailable";
    }
    if (rec->err[0]) {
        return rec->err;
    }
    return rec->enc ? rec->enc->err : "the recorder is unavailable";
}

void vshot_rec_free(VshotRec *rec) {
    if (!rec) {
        return;
    }
    if (rec->fmt) {
        if (rec->fmt->pb) {
            api->io_closep(&rec->fmt->pb);
        }
        api->format_free_context(rec->fmt);
        rec->fmt = NULL;
    }
    if (rec->audio) {
        vshot_audio_enc_destroy(rec->audio);
        rec->audio = NULL;
    }
    if (rec->enc) {
        vshot_av_enc_destroy(rec->enc);
        rec->enc = NULL;
    }
    free(rec);
}

// Whether the recorder can run at all: libavcodec + libavutil + libavformat.
int vshot_rec_available(void) {
    api = load_api();
    return api != NULL && api->format_loaded;
}

const char *vshot_rec_load_error(void) {
    if (create_error[0]) {
        return create_error;
    }
    if (api_error[0]) {
        return api_error;
    }
    return "";
}

// The recorder-level audio entry points.  The encoder lives inside the
// VshotRec, because its stream has to exist in the muxer before the header is
// written and its packets go to the same AVFormatContext as the video's.
//
// Queues interleaved float samples from the microphone.
int vshot_rec_audio_feed(VshotRec *rec, const float *samples, int frames) {
    if (!rec || !rec->audio) {
        return -1;
    }
    return vshot_audio_enc_feed(rec->audio, samples, frames);
}

// Encodes and muxes everything queued so far.
int vshot_rec_audio_pump(VshotRec *rec) {
    if (!rec || !rec->audio) {
        return -1;
    }
    return vshot_audio_enc_pump(rec->audio, rec, 0);
}

// Whether a soundtrack is being recorded, and in what shape.
int vshot_rec_audio_rate(VshotRec *rec) {
    return rec && rec->audio ? rec->audio->rate : 0;
}

int vshot_rec_audio_channels(VshotRec *rec) {
    return rec && rec->audio ? rec->audio->channels : 0;
}

const char *vshot_rec_audio_error(VshotRec *rec) {
    if (!rec || !rec->audio) {
        return "";
    }
    return rec->audio->err;
}

unsigned vshot_rec_version(void) {
    api = load_api();
    return api ? api->codec_version() : 0;
}

// Whether a hardware backend can be opened on this machine at all.  This is
// what `--encoder-backend auto` asks: it opens the backend's device (a render
// node, a CUDA device) and looks the encoder up, then closes both.  No frames
// are encoded, so the probe is cheap and side-effect free.  Returns 0 on
// success, -1 with `err` filled in otherwise.  It is the same work
// `open_encoder` does up to the point a device is in hand, so a backend the
// probe accepts is one a recording will accept too (modulo the encode itself).
int vshot_av_enc_backend_probe(int backend, char *err, size_t err_len) {
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    api = load_api();
    if (!api) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "%s", vshot_av_enc_load_error());
        }
        return -1;
    }
    AVBufferRef *device = NULL;
    int ret;
    if (backend == VSHOT_BACKEND_NVENC) {
        const char *index = getenv("VSHOT_NVENC_DEVICE");
        ret = api->hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, index, NULL, 0);
        if (ret < 0) {
            char text[AV_ERROR_MAX_STRING_SIZE] = {0};
            api->strerror(ret, text, sizeof(text));
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "no CUDA device for NVENC: %s",
                         text[0] ? text : "the CUDA hwcontext could not be created");
            }
            return -1;
        }
    } else {
        ret = api->hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128",
                                       NULL, 0);
        if (ret < 0) {
            char text[AV_ERROR_MAX_STRING_SIZE] = {0};
            api->strerror(ret, text, sizeof(text));
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "no VAAPI device: %s",
                         text[0] ? text : "/dev/dri/renderD128 could not be opened");
            }
            return -1;
        }
    }
    api->buffer_unref(&device);
    // The device opened; the encoder has to exist in this build too.
    char name[64];
    const AVCodec *encoder =
        api->find_encoder_by_name(backend_encoder_name(backend, "h264", name, sizeof(name)));
    if (!encoder) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "this ffmpeg build has no %s encoder", name);
        }
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The replay recorder: the same encoder, a ring instead of a file
// ---------------------------------------------------------------------------

// Starts a replay session: an encoder in ring mode, no muxer and no file.
// `window_ms` is how much history the ring keeps; `gop_frames` is the key-frame
// distance (a bounded GOP keeps the ring small and gives every save a place to
// start from); `fps` sizes the ring for the rate the session really runs.
// `dmabuf` selects the zero-copy input, exactly as for a recording.
VshotRec *vshot_rec_start_replay(int width, int height, const char *codec, int qp, int dmabuf,
                                 unsigned fourcc, int mic_rate, int mic_channels,
                                 int64_t window_ms, int gop_frames, int fps, int backend) {
    api = load_api();
    if (!api) {
        return NULL;
    }
    if (!codec || !codec[0] || width <= 0 || height <= 0) {
        snprintf(create_error, sizeof(create_error), "the replay was given an empty shape");
        return NULL;
    }
    // As in `rec_start`: NVENC records the software path, never a dma-buf.
    if (dmabuf && backend == VSHOT_BACKEND_NVENC) {
        dmabuf = 0;
        fourcc = 0;
    }
    if (window_ms <= 0) {
        snprintf(create_error, sizeof(create_error), "the replay window has to be positive");
        return NULL;
    }
    VshotRec *rec = calloc(1, sizeof(VshotRec));
    if (!rec) {
        snprintf(create_error, sizeof(create_error), "out of memory for the replay");
        return NULL;
    }
    if (mic_rate > 0 && mic_channels > 0) {
        char detail[256] = {0};
        rec->audio = vshot_audio_enc_create(mic_rate, mic_channels, detail, sizeof(detail));
        if (!rec->audio) {
            snprintf(create_error, sizeof(create_error), "%s", detail);
            free(rec);
            return NULL;
        }
    }
    // The encoder opens with a bounded GOP; the ring is created before the
    // encoder so a packet can never arrive without a place to go.
    VshotReplay *ring = replay_create(window_ms, fps);
    if (!ring) {
        snprintf(create_error, sizeof(create_error), "out of memory for the replay ring");
        vshot_rec_free(rec);
        return NULL;
    }
    rec->enc = dmabuf
                   ? vshot_av_enc_create_dmabuf(width, height, codec, qp, gop_frames, fourcc,
                                                backend)
                   : vshot_av_enc_create(width, height, codec, qp, gop_frames, backend);
    if (!rec->enc) {
        char detail[sizeof(create_error)];
        snprintf(detail, sizeof(detail), "%s", vshot_av_enc_load_error());
        snprintf(create_error, sizeof(create_error), "%s", detail);
        replay_destroy(ring);
        vshot_rec_free(rec);
        return NULL;
    }
    rec->enc->replay = ring;
    rec->enc->replay_next_ms = 0;
    return rec;
}

// How much history the ring currently holds, in seconds.
double vshot_rec_replay_span(VshotRec *rec) {
    if (!rec || !rec->enc || !rec->enc->replay) {
        return 0.0;
    }
    VshotReplay *r = rec->enc->replay;
    if (r->count == 0) {
        return 0.0;
    }
    return (double)(r->newest_ms - r->oldest_ms) / 1000.0;
}

// One entry of a save, kept so the ring can be walked in time order.
typedef struct ReplayPick {
    int index;
    int64_t ms;
} ReplayPick;

static int replay_pick_cmp(const void *a, const void *b) {
    int64_t x = ((const ReplayPick *)a)->ms;
    int64_t y = ((const ReplayPick *)b)->ms;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Writes the last `seconds` of the ring to a new MP4 at `path`, as a stream
// copy: the packets are handed to a fresh muxer unchanged, so the save costs a
// remux and no re-encode.  The start is aligned to a key frame at or before
// the requested edge, because a file whose first video packet is not a key
// frame shows nothing until the next one.  `out_ms`, when not NULL, receives
// the file's real length in milliseconds (the requested edge rounded back to a
// key frame can make it a little longer).
int vshot_rec_replay_save(VshotRec *rec, const char *path, int seconds, int64_t *out_ms) {
    if (out_ms) {
        *out_ms = 0;
    }
    if (!rec || !rec->enc) {
        return -1;
    }
    VshotReplay *r = rec->enc->replay;
    if (!r || r->count == 0) {
        snprintf(rec->err, sizeof(rec->err), "the replay buffer is empty; nothing to save");
        return -1;
    }
    if (!api->format_loaded) {
        snprintf(rec->err, sizeof(rec->err),
                 "libavformat is not installed, so a replay cannot be written (install ffmpeg)");
        return -1;
    }
    if (!path || !path[0]) {
        snprintf(rec->err, sizeof(rec->err), "the replay save was given no path");
        return -1;
    }
    if (seconds <= 0) {
        seconds = (int)(r->window_ms / 1000);
    }
    // The edge the save wants: `seconds` before the newest packet.  The file
    // then starts at the last key frame at or before that edge, so it holds at
    // least `seconds` and is decodable from its first byte.
    int64_t want_ms = r->newest_ms - (int64_t)seconds * 1000;
    int chosen = -1;
    for (int i = 0; i < r->count; i++) {
        VshotReplayPkt *e = &r->pkts[(r->head + i) % r->cap];
        if (!e->is_audio && e->is_key && e->ms <= want_ms) {
            chosen = i;
        }
    }
    if (chosen < 0) {
        // No key frame that old (the window is short, or the save asks for
        // more than the ring holds): the earliest key frame is the answer.
        for (int i = 0; i < r->count; i++) {
            VshotReplayPkt *e = &r->pkts[(r->head + i) % r->cap];
            if (!e->is_audio && e->is_key) {
                chosen = i;
                break;
            }
        }
    }
    if (chosen < 0) {
        // Nothing is a key frame (should not happen with a bounded GOP): fall
        // back to the oldest entry, so a save still produces a file.
        chosen = 0;
    }
    int64_t base_ms = r->pkts[(r->head + chosen) % r->cap].ms;

    // Everything from the base onward, in time order (a hardware encoder's
    // delay can leave an audio packet just ahead of a video one).
    if (r->count <= 0 || r->count > r->cap) {
        snprintf(rec->err, sizeof(rec->err), "the replay ring is in an inconsistent state");
        return -1;
    }
    ReplayPick *picks = calloc((size_t)r->count, sizeof(*picks));
    if (!picks) {
        snprintf(rec->err, sizeof(rec->err), "out of memory ordering the replay save");
        return -1;
    }
    int npick = 0;
    for (int i = 0; i < r->count; i++) {
        int idx = (r->head + i) % r->cap;
        if (r->pkts[idx].ms >= base_ms) {
            picks[npick].index = idx;
            picks[npick].ms = r->pkts[idx].ms;
            npick++;
        }
    }
    qsort(picks, (size_t)npick, sizeof(*picks), replay_pick_cmp);

    // The container: the same shape the recorder builds, from the same codec
    // parameters, so the file is what `record` would have written.
    AVFormatContext *fmt = NULL;
    if (api->format_alloc_output(&fmt, NULL, "mp4", path) < 0 || !fmt) {
        snprintf(rec->err, sizeof(rec->err), "could not open an MP4 muxer for %s", path);
        free(picks);
        return -1;
    }
    AVStream *vstream = api->format_new_stream(fmt, NULL);
    if (!vstream) {
        snprintf(rec->err, sizeof(rec->err), "could not add a video stream to the replay's MP4");
        api->format_free_context(fmt);
        free(picks);
        return -1;
    }
    vstream->time_base = (AVRational){1, 1000};
    if (api->parameters_from_context(vstream->codecpar, rec->enc->ctx) < 0) {
        snprintf(rec->err, sizeof(rec->err), "the MP4 muxer refused the codec parameters");
        api->format_free_context(fmt);
        free(picks);
        return -1;
    }
    vstream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vstream->codecpar->width = rec->enc->width;
    vstream->codecpar->height = rec->enc->height;
    AVStream *astream = NULL;
    if (rec->audio) {
        astream = api->format_new_stream(fmt, NULL);
        if (!astream) {
            snprintf(rec->err, sizeof(rec->err), "could not add an audio stream to the replay's MP4");
            api->format_free_context(fmt);
            free(picks);
            return -1;
        }
        astream->time_base = (AVRational){1, rec->audio->rate};
        if (api->parameters_from_context(astream->codecpar, rec->audio->ctx) < 0) {
            snprintf(rec->err, sizeof(rec->err), "the MP4 muxer refused the audio codec parameters");
            api->format_free_context(fmt);
            free(picks);
            return -1;
        }
        astream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        astream->codecpar->sample_rate = rec->audio->rate;
    }
    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        if (api->io_open(&fmt->pb, path, AVIO_FLAG_WRITE) < 0) {
            snprintf(rec->err, sizeof(rec->err), "could not open %s for writing", path);
            api->format_free_context(fmt);
            free(picks);
            return -1;
        }
    }
    if (api->format_write_header(fmt, NULL) < 0) {
        snprintf(rec->err, sizeof(rec->err), "writing the replay's MP4 header failed");
        if (fmt->pb) {
            api->io_closep(&fmt->pb);
        }
        api->format_free_context(fmt);
        free(picks);
        return -1;
    }
    AVRational vtb = vstream->time_base;
    AVRational atb = astream ? astream->time_base : (AVRational){1, 1000};
    AVRational ms = {1, 1000};
    int written = 0;
    for (int i = 0; i < npick; i++) {
        VshotReplayPkt *e = &r->pkts[picks[i].index];
        int64_t rel = e->ms - base_ms;
        if (rel < 0) {
            rel = 0;
        }
        e->pkt->stream_index = e->is_audio ? astream->index : vstream->index;
        AVRational tb = e->is_audio ? atb : vtb;
        e->pkt->pts = e->pkt->dts = api->rescale_q(rel, ms, tb);
        e->pkt->duration = api->rescale_q(e->dur_ms, ms, tb);
        if (api->format_write_frame(fmt, e->pkt) < 0) {
            snprintf(rec->err, sizeof(rec->err),
                     "writing a replay packet to the MP4 muxer failed");
            if (fmt->pb) {
                api->io_closep(&fmt->pb);
            }
            api->format_free_context(fmt);
            free(picks);
            return -1;
        }
        written++;
    }
    if (api->format_write_trailer(fmt) < 0) {
        snprintf(rec->err, sizeof(rec->err), "writing the replay's MP4 trailer failed");
        if (fmt->pb) {
            api->io_closep(&fmt->pb);
        }
        api->format_free_context(fmt);
        free(picks);
        return -1;
    }
    if (fmt->pb) {
        api->io_closep(&fmt->pb);
    }
    api->format_free_context(fmt);
    // The length the file really has: the last packet's end, on the same
    // millisecond timeline the save wrote.  Computed before `picks` is freed.
    if (out_ms) {
        int64_t end_ms = 0;
        for (int i = 0; i < npick; i++) {
            VshotReplayPkt *e = &r->pkts[picks[i].index];
            int64_t rel = e->ms - base_ms + e->dur_ms;
            if (rel > end_ms) {
                end_ms = rel;
            }
        }
        *out_ms = end_ms;
    }
    free(picks);
    r->saved++;
    return written;
}
