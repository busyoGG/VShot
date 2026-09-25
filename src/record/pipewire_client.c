// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

/*
 * The portal's PipeWire stream, as a C client.
 *
 * The XDG portal does not hand frames over itself.  `OpenPipeWireRemote`
 * returns a file descriptor to the compositor's PipeWire connection, and the
 * frames come from a node on it, so a recording that goes through the portal
 * needs a PipeWire client: connect to that node, agree on a video format, and
 * read the buffers the compositor renders into.  That work is C — libpipewire
 * is a C library whose callbacks arrive on its own loop thread — and this file
 * is that client, wrapped in an API small enough for Rust to call.
 *
 * Four things shape it:
 *
 *   * **libpipewire is `dlopen`ed, not linked.**  Screenshots and the
 *     compositor's own recording protocols do not need PipeWire at all, so the
 *     binary must not acquire a hard dependency on it.  The headers are a
 *     build-time dependency (this file cannot be compiled without them), the
 *     library is not: `vshot_pw_available()` answers whether it is here, and
 *     `vshot_pw_load_error()` names the entry point that is missing when it is
 *     not.
 *
 *   * **The thread loop runs the protocol; the caller waits.**  A
 *     `pw_thread_loop` owns the connection: its thread negotiates the format,
 *     receives buffers and runs the callbacks.  The caller (vshot's recording
 *     loop) waits on a condition variable for a frame and asks for the next one
 *     when it is done with the current one.  One frame in hand at a time is
 *     what the recording loop wants anyway, and it is what keeps the buffer's
 *     lifetime unambiguous.
 *
 *   * **The compositor decides when frames appear.**  A screen-cast node is
 *     damage-driven: it produces a frame when the screen it is casting changes,
 *     and nothing at all when it does not.  A wait that returns no frame is
 *     therefore normal, not a failure, and the caller is told the difference:
 *     `vshot_pw_next` returns 1 for "nothing yet" and -1 for "the stream
 *     ended".
 *
 *   * **Two buffer shapes, one per encoder path.**  A dma-buf frame goes to
 *     the GPU encoder without a copy; a memory frame (the SHM shape,
 *     `SPA_DATA_MemFd`) is read through the mapping PipeWire set up.  Which one
 *     arrives is the compositor's answer to the format asked for: asking with a
 *     modifier (`SPA_FORMAT_VIDEO_modifier`) gets dma-buf buffers, asking
 *     without one gets memory.
 *
 * The ABI below is mirrored field for field in `src/record/pipewire.rs`; the
 * two files have to change together.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>

/* `DRM_FORMAT_MOD_LINEAR` from drm_fourcc.h, spelled out here so the build does
 * not depend on libdrm's headers for one constant.  A linear modifier is the
 * one buffer layout every VAAPI driver can import, which is why it is the only
 * modifier this client asks for. */
#define VSHOT_MOD_LINEAR 0ull

/* `DRM_FORMAT_ARGB8888` / `DRM_FORMAT_XRGB8888` from drm_fourcc.h, for the same
 * reason.  The SPA format names the byte order in memory; these are the DRM
 * names for the same two layouts. */
#define VSHOT_FOURCC_ARGB8888 0x34325241u /* 'AR24' */
#define VSHOT_FOURCC_XRGB8888 0x34325258u /* 'XR24' */

/* The frame kinds of the ABI. */
#define VSHOT_PW_KIND_NONE 0
#define VSHOT_PW_KIND_DMABUF 1
#define VSHOT_PW_KIND_MEMORY 2

/* How long `vshot_pw_open` waits for each round of the format negotiation, in
 * whole seconds.  The negotiation is a handful of round trips over the local
 * socket, so a second is already generous; waiting in slices is what lets the
 * deadline be checked from the caller's own thread. */
#define VSHOT_PW_NEGOTIATION_SLICE_SEC 1

/* One frame, as the Rust side declares it.  The order of these fields is the
 * ABI: `the_frame_layout_is_the_c_structs` in Rust asserts every offset. */
typedef struct vshot_pw_frame {
	int kind;                  /* 0 none, 1 dma-buf, 2 memory */
	int fd;                    /* the dma-buf, for a dma-buf frame */
	uint32_t fourcc;           /* the DRM fourcc of the pixels */
	uint64_t modifier;         /* the dma-buf's modifier */
	int offset;                /* byte offset of the pixels in the buffer */
	int stride;                /* bytes per row */
	const unsigned char *data; /* the pixels, for a memory frame */
	uint64_t size;             /* how many bytes of them */
	int width;                 /* the frame's size in pixels */
	int height;
	uint32_t spa_format; /* the SPA_VIDEO_FORMAT_* the stream agreed on */
	int64_t seq;         /* the frames seen so far */
	int64_t time_ns;     /* when the buffer was received, monotonic */
} VshotPwFrame;

typedef struct vshot_pw VshotPw;

/*
 * The libpipewire entry points, resolved by name.
 *
 * Declaring them as pointers rather than calling the header's prototypes is
 * what keeps libpipewire out of the binary's dynamic table: the headers still
 * supply every type, the symbols come from dlopen.  The `spa_pod_*` and
 * `spa_pod_builder_*` families are `static inline` in their headers, so they
 * are called directly.
 */
struct vshot_pw_api {
	void (*init)(int *argc, char ***argv);
	struct pw_thread_loop *(*thread_loop_new)(const char *name, const struct spa_dict *props);
	void (*thread_loop_destroy)(struct pw_thread_loop *loop);
	int (*thread_loop_start)(struct pw_thread_loop *loop);
	void (*thread_loop_stop)(struct pw_thread_loop *loop);
	void (*thread_loop_lock)(struct pw_thread_loop *loop);
	void (*thread_loop_unlock)(struct pw_thread_loop *loop);
	struct pw_loop *(*thread_loop_get_loop)(struct pw_thread_loop *loop);
	void (*thread_loop_signal)(struct pw_thread_loop *loop, bool wait_for_accept);
	int (*thread_loop_timed_wait)(struct pw_thread_loop *loop, int wait_max_sec);
	struct pw_context *(*context_new)(struct pw_loop *loop, struct pw_properties *props,
					  size_t user_data_size);
	void (*context_destroy)(struct pw_context *context);
	struct pw_core *(*context_connect)(struct pw_context *context,
					   struct pw_properties *props, size_t user_data_size);
	struct pw_core *(*context_connect_fd)(struct pw_context *context, int fd,
					      struct pw_properties *props, size_t user_data_size);
	void (*core_disconnect)(struct pw_core *core);
	struct pw_properties *(*properties_new)(const char *key, ...);
	struct pw_stream *(*stream_new)(struct pw_core *core, const char *name,
					struct pw_properties *props);
	void (*stream_destroy)(struct pw_stream *stream);
	int (*stream_connect)(struct pw_stream *stream, enum pw_direction direction,
			      uint32_t target_id, enum pw_stream_flags flags,
			      const struct spa_pod *const *params, uint32_t n_params);
	struct pw_buffer *(*stream_dequeue_buffer)(struct pw_stream *stream);
	int (*stream_queue_buffer)(struct pw_stream *stream, struct pw_buffer *buffer);
	void (*stream_add_listener)(struct pw_stream *stream, struct spa_hook *listener,
				    const struct pw_stream_events *events, void *data);
	enum pw_stream_state (*stream_get_state)(struct pw_stream *stream, const char **error);
	const char *(*stream_state_as_string)(enum pw_stream_state state);
};

/* One client.  Everything the two threads share is under `lock`: the format the
 * negotiation settled on, the frame waiting to be taken, and the failure (if
 * any). */
struct vshot_pw {
	const struct vshot_pw_api *api;
	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	/* PipeWire keeps a *pointer* to the events structure for the life of the
	 * stream — `pw_stream_add_listener` stores it in the stream's callback
	 * list and in its realtime callback slot — so it has to live as long as
	 * the stream does.  On the stack it would be a dangling pointer the
	 * moment the function that connected the stream returned, and the first
	 * process callback would jump into whatever happened to be there. */
	struct pw_stream_events events;
	bool loop_started;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	/* The newest frame the source produced that the caller has not taken
	 * yet.  A frame arriving while one is still waiting replaces it: a
	 * recording wants the screen as it is now, not every step it took to
	 * get there. */
	struct pw_buffer *slot;
	/* The frame the caller is holding, until its next call. */
	struct pw_buffer *held;

	/* The negotiation's answer. */
	bool negotiated;
	bool dmabuf;
	uint32_t width;
	uint32_t height;
	uint32_t spa_format;
	uint64_t modifier;

	/* Set when the stream cannot continue, with `error` saying why. */
	bool failed;
	bool ended;
	char error[512];

	int64_t sequence;
};

/*
 * The shared state, as seen from a source callback or from the caller.
 */

static const struct vshot_pw_api *vshot_pw_api_load(char *error, size_t error_len);

/* Defined with the rest of the entry points; `vshot_pw_open` unwinds through
 * it, so it has to be declared before it. */
void vshot_pw_close(VshotPw *pw);

static void vshot_pw_set_error(VshotPw *pw, const char *format, ...)
{
	va_list arguments;

	pthread_mutex_lock(&pw->lock);
	if (pw->error[0] == '\0') {
		va_start(arguments, format);
		vsnprintf(pw->error, sizeof(pw->error), format, arguments);
		va_end(arguments);
	}
	pw->failed = true;
	pthread_cond_broadcast(&pw->cond);
	pthread_mutex_unlock(&pw->lock);

	/* The caller may be waiting inside the thread loop rather than on the
	 * condition variable; waking it is how a failure inside a callback
	 * becomes an answer to `vshot_pw_open`. */
	if (pw->loop != NULL)
		pw->api->thread_loop_signal(pw->loop, false);
}

/* Hand a buffer back to the source so it can render into it again.  Only
 * called with `lock` held: the stream's recycle queue is a single-producer
 * ring, and both the process callback and the caller return buffers, so this
 * lock is what keeps them from being two producers. */
static void vshot_pw_recycle_locked(VshotPw *pw, struct pw_buffer *buffer)
{
	if (buffer != NULL && pw->stream != NULL)
		pw->api->stream_queue_buffer(pw->stream, buffer);
}

/* Writes a sentence into the caller's buffer.  The caller may ask for no
 * message at all (`err_len` of zero), which is not an error here. */
static void vshot_pw_error_out(char *err, int err_len, const char *format, ...)
{
	va_list arguments;

	if (err == NULL || err_len <= 0)
		return;
	va_start(arguments, format);
	vsnprintf(err, (size_t)err_len, format, arguments);
	va_end(arguments);
}

static void vshot_pw_wake(VshotPw *pw)
{
	pthread_cond_broadcast(&pw->cond);
	if (pw->loop != NULL)
		pw->api->thread_loop_signal(pw->loop, false);
}

static void vshot_pw_on_state_changed(void *data, enum pw_stream_state old,
				      enum pw_stream_state state, const char *error)
{
	VshotPw *pw = data;

	(void)old;
	if (state == PW_STREAM_STATE_ERROR) {
		vshot_pw_set_error(pw, "the screen-cast stream failed: %s",
				   error != NULL ? error : "the compositor did not say why");
		return;
	}
	if (state == PW_STREAM_STATE_UNCONNECTED) {
		pthread_mutex_lock(&pw->lock);
		if (pw->negotiated)
			pw->ended = true;
		else
			pw->failed = true;
		if (pw->error[0] == '\0' && !pw->negotiated)
			snprintf(pw->error, sizeof(pw->error),
				 "the screen-cast stream was closed before it reported a video "
				 "format");
		vshot_pw_wake(pw);
		pthread_mutex_unlock(&pw->lock);
	}
}

/* The format is the negotiation's answer: this is where the frame size and the
 * pixel layout become known.  A pod that still says DONT_FIXATE is not the
 * answer yet — a compositor's portal reads that as "you pick a buffer and tell
 * me", and reports the fixated format in a second round. */
static void vshot_pw_on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	VshotPw *pw = data;
	struct spa_video_info_raw info;
	const struct spa_pod_prop *property;
	struct spa_pod *values;
	uint32_t count = 0;
	uint32_t choice = 0;
	int64_t modifier = 0;
	bool dmabuf;

	if (id != SPA_PARAM_Format || param == NULL)
		return;
	if (spa_format_video_raw_parse(param, &info) < 0)
		return;
	if (info.size.width == 0 || info.size.height == 0)
		return;

	property = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
	if (property != NULL && (property->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) != 0)
		return;

	dmabuf = false;
	if (property != NULL) {
		values = spa_pod_get_values(&property->value, &count, &choice);
		/* The first value is the default, which is what a source uses
		 * when it does not pick from the alternatives. */
		if (count == 0 || values == NULL || spa_pod_get_long(values, &modifier) < 0)
			return;
		dmabuf = true;
	}

	pthread_mutex_lock(&pw->lock);
	pw->width = info.size.width;
	pw->height = info.size.height;
	pw->spa_format = (uint32_t)info.format;
	pw->dmabuf = dmabuf;
	pw->modifier = dmabuf ? (uint64_t)modifier : 0;
	pw->negotiated = true;
	vshot_pw_wake(pw);
	pthread_mutex_unlock(&pw->lock);
}

/* A buffer is ready.  Taking it here, in the source's own callback, is the
 * documented place for it: the buffer sits in the stream's dequeue queue
 * exactly while this callback runs. */
static void vshot_pw_on_process(void *data)
{
	VshotPw *pw = data;
	struct pw_buffer *buffer;
	struct pw_buffer *dropped = NULL;

	pthread_mutex_lock(&pw->lock);
	if (pw->stream == NULL || pw->failed || pw->ended) {
		pthread_mutex_unlock(&pw->lock);
		return;
	}
	buffer = pw->api->stream_dequeue_buffer(pw->stream);
	if (buffer == NULL) {
		pthread_mutex_unlock(&pw->lock);
		return;
	}
	if (pw->slot != NULL)
		dropped = pw->slot;
	pw->slot = buffer;
	pw->sequence++;
	vshot_pw_wake(pw);
	vshot_pw_recycle_locked(pw, dropped);
	pthread_mutex_unlock(&pw->lock);
}

/* Fills `out` from a buffer the stream handed over.  The size and the layout
 * come from the negotiated format; the offsets and the stride come from the
 * buffer itself, which is where a padded row or a plane offset shows up. */
static void vshot_pw_describe(VshotPw *pw, struct pw_buffer *buffer, VshotPwFrame *out)
{
	struct spa_data *data = &buffer->buffer->datas[0];
	struct spa_chunk *chunk = data->chunk;
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	out->width = (int)pw->width;
	out->height = (int)pw->height;
	out->spa_format = pw->spa_format;
	/* The DRM name for the byte order the SPA format stands for.  vshot's
	 * encoder imports the two layouts a screen cast produces; anything else
	 * is refused there, with the format named. */
	out->fourcc = pw->spa_format == SPA_VIDEO_FORMAT_BGRx ? VSHOT_FOURCC_XRGB8888
							      : VSHOT_FOURCC_ARGB8888;
	out->seq = pw->sequence;
	out->time_ns = (int64_t)now.tv_sec * 1000000000ll + (int64_t)now.tv_nsec;

	if (data->type == SPA_DATA_DmaBuf) {
		out->kind = VSHOT_PW_KIND_DMABUF;
		out->fd = data->fd;
		out->modifier = pw->modifier;
		out->offset = (int)chunk->offset;
		out->stride = (int)chunk->stride;
		out->data = NULL;
		out->size = (uint64_t)chunk->size;
		return;
	}

	out->kind = VSHOT_PW_KIND_MEMORY;
	out->fd = -1;
	out->modifier = 0;
	out->offset = 0;
	out->stride = (int)chunk->stride;
	out->size = (uint64_t)chunk->size;
	if (data->data == NULL) {
		out->kind = VSHOT_PW_KIND_NONE;
		out->data = NULL;
		out->size = 0;
		return;
	}
	/* `data` is already offset by the buffer's map offset, so the chunk's
	 * own offset is all that is left between it and the pixels. */
	out->data = data->data + chunk->offset;
}

/*
 * The formats this client asks for.
 *
 * Two pods per layout: one with a modifier (dma-buf, the zero-copy shape the
 * encoder imports) and one without (memory, the shape that works anywhere).
 * The opaque layout comes first, because a screen has no alpha to keep; the
 * layout with alpha follows for a window, which can have one.
 *
 * The modifier choice repeats the linear modifier on purpose: the protocol
 * reads the first value as the default and the rest as the alternatives, so a
 * single value would leave the compositor free to allocate whatever its driver
 * prefers, and a compressed or tiled layout is not something every encoder can
 * import.
 *
 * The size and the frame rate are ranges: the size is the compositor's to
 * choose — it knows what is being cast — and the frame rate is a limit rather
 * than a promise, because a screen cast produces a frame when the screen
 * changes.
 */
static struct spa_pod *vshot_pw_build_format(struct spa_pod_builder *builder, uint32_t format,
					     bool dmabuf, uint32_t fps)
{
	struct spa_pod_frame object;
	struct spa_pod_frame choice;
	struct spa_rectangle default_size = SPA_RECTANGLE(1920, 1080);
	struct spa_rectangle minimum_size = SPA_RECTANGLE(1, 1);
	struct spa_rectangle maximum_size = SPA_RECTANGLE(16384, 16384);
	struct spa_fraction variable = SPA_FRACTION(0, 1);
	struct spa_fraction default_fps = SPA_FRACTION(fps, 1);
	struct spa_fraction minimum_fps = SPA_FRACTION(1, 1);
	struct spa_fraction maximum_fps = SPA_FRACTION(fps, 1);

	spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
	if (dmabuf) {
		spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier,
				     SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
		spa_pod_builder_long(builder, (int64_t)VSHOT_MOD_LINEAR);
		spa_pod_builder_long(builder, (int64_t)VSHOT_MOD_LINEAR);
		spa_pod_builder_pop(builder, &choice);
	}
	spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_size,
			    SPA_POD_CHOICE_RANGE_Rectangle(&default_size, &minimum_size,
							   &maximum_size),
			    0);
	spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable), 0);
	spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_maxFramerate,
			    SPA_POD_CHOICE_RANGE_Fraction(&default_fps, &minimum_fps, &maximum_fps),
			    0);
	return spa_pod_builder_pop(builder, &object);
}

/*
 * The entry points, one for one with the declarations in `pipewire.rs`.
 */

int vshot_pw_available(void)
{
	return vshot_pw_api_load(NULL, 0) != NULL ? 1 : 0;
}

const char *vshot_pw_load_error(void)
{
	static char error[512];

	if (vshot_pw_api_load(error, sizeof(error)) != NULL)
		error[0] = '\0';
	else if (error[0] == '\0')
		snprintf(error, sizeof(error), "libpipewire could not be loaded");
	return error;
}

VshotPw *vshot_pw_open(int fd, uint32_t node_id, int allow_dmabuf, int fps, int timeout_ms,
		       char *err, int err_len)
{
	const struct vshot_pw_api *api = vshot_pw_api_load(err, (size_t)err_len);
	struct spa_pod_builder builder;
	struct spa_pod *params[4];
	uint8_t storage[4][1024];
	uint32_t n_params = 0;
	uint32_t rate = fps > 0 ? (uint32_t)fps : 60;
	enum pw_stream_flags flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS;
	struct pw_properties *properties;
	struct timespec deadline;
	bool locked = false;
	bool done;
	VshotPw *pw;

	if (err != NULL && err_len > 0)
		err[0] = '\0';
	if (api == NULL)
		return NULL;
	if (timeout_ms <= 0)
		timeout_ms = 15000;
	/* The portal hands over a real descriptor.  One that is not open at all
	 * — a stale number, a bug on the other side — would fail deep inside the
	 * connection and take the process with it, which is worth refusing here
	 * with a sentence instead. */
	if (fd >= 0 && fcntl(fd, F_GETFD) < 0) {
		vshot_pw_error_out(err, err_len,
				   "the portal's PipeWire file descriptor (%d) is not open: %s", fd,
				   strerror(errno));
		return NULL;
	}

	pw = (VshotPw *)calloc(1, sizeof(*pw));
	if (pw == NULL) {
		vshot_pw_error_out(err, err_len, "out of memory for the PipeWire client");
		return NULL;
	}
	pw->api = api;
	pthread_mutex_init(&pw->lock, NULL);
	pthread_cond_init(&pw->cond, NULL);

	api->init(NULL, NULL);

	pw->loop = api->thread_loop_new("vshot-portal", NULL);
	if (pw->loop == NULL) {
		vshot_pw_error_out(err, err_len, "the PipeWire thread loop could not be created");
		goto fail;
	}

	api->thread_loop_lock(pw->loop);
	locked = true;

	/* The properties say what this stream is for; a session that has rules
	 * for screen casting (the portal's own, or a policy daemon's) reads
	 * them. */
	properties = api->properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
					 PW_KEY_MEDIA_ROLE, "Screen", NULL);
	pw->context = api->context_new(api->thread_loop_get_loop(pw->loop), properties, 0);
	if (pw->context == NULL) {
		vshot_pw_error_out(err, err_len,
				   "the PipeWire context could not be created: %s", strerror(errno));
		goto fail;
	}

	/* The portal hands over a connection to the compositor's own PipeWire
	 * instance, which is what `fd` is. */
	if (fd >= 0)
		pw->core = api->context_connect_fd(pw->context, fd, NULL, 0);
	else
		pw->core = api->context_connect(pw->context, NULL, 0);
	if (pw->core == NULL) {
		vshot_pw_error_out(err, err_len,
				   "the portal's PipeWire connection could not be used: %s",
				   strerror(errno));
		goto fail;
	}

	pw->stream = api->stream_new(pw->core, "vshot-capture", api->properties_new(NULL, NULL));
	if (pw->stream == NULL) {
		vshot_pw_error_out(err, err_len, "the PipeWire stream could not be created: %s",
				   strerror(errno));
		goto fail;
	}

	memset(&pw->events, 0, sizeof(pw->events));
	pw->events.version = PW_VERSION_STREAM_EVENTS;
	pw->events.state_changed = vshot_pw_on_state_changed;
	pw->events.param_changed = vshot_pw_on_param_changed;
	pw->events.process = vshot_pw_on_process;
	api->stream_add_listener(pw->stream, &pw->stream_listener, &pw->events, pw);

	if (allow_dmabuf) {
		spa_pod_builder_init(&builder, storage[n_params], sizeof(storage[0]));
		params[n_params] = vshot_pw_build_format(&builder, SPA_VIDEO_FORMAT_BGRx, true, rate);
		n_params++;
		spa_pod_builder_init(&builder, storage[n_params], sizeof(storage[0]));
		params[n_params] = vshot_pw_build_format(&builder, SPA_VIDEO_FORMAT_BGRA, true, rate);
		n_params++;
	}
	spa_pod_builder_init(&builder, storage[n_params], sizeof(storage[0]));
	params[n_params] = vshot_pw_build_format(&builder, SPA_VIDEO_FORMAT_BGRx, false, rate);
	n_params++;
	spa_pod_builder_init(&builder, storage[n_params], sizeof(storage[0]));
	params[n_params] = vshot_pw_build_format(&builder, SPA_VIDEO_FORMAT_BGRA, false, rate);
	n_params++;

	if (api->stream_connect(pw->stream, PW_DIRECTION_INPUT, node_id, flags,
				(const struct spa_pod *const *)params, n_params) < 0) {
		vshot_pw_error_out(err, err_len, "the screen-cast node could not be connected: %s",
				   strerror(errno));
		goto fail;
	}

	api->thread_loop_unlock(pw->loop);
	locked = false;

	if (api->thread_loop_start(pw->loop) < 0) {
		vshot_pw_error_out(err, err_len, "the PipeWire thread loop could not be started: %s",
				   strerror(errno));
		goto fail;
	}
	pw->loop_started = true;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	/* Waiting in slices: the callbacks that end the wait run on the thread
	 * loop, so the deadline is checked from here rather than inside. */
	for (;;) {
		struct timespec now;

		pthread_mutex_lock(&pw->lock);
		done = pw->negotiated || pw->failed || pw->ended;
		pthread_mutex_unlock(&pw->lock);
		if (done)
			break;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
			vshot_pw_error_out(err, err_len,
					   "the compositor did not report a video format within %d "
					   "ms",
					   timeout_ms);
			goto fail;
		}

		api->thread_loop_lock(pw->loop);
		api->thread_loop_timed_wait(pw->loop, VSHOT_PW_NEGOTIATION_SLICE_SEC);
		api->thread_loop_unlock(pw->loop);
	}

	pthread_mutex_lock(&pw->lock);
	done = pw->negotiated && pw->width > 0 && pw->height > 0;
	if (!done && err != NULL && err_len > 0)
		snprintf(err, (size_t)err_len, "%s",
			 pw->error[0] != '\0'
				 ? pw->error
				 : "the screen-cast stream ended before it reported a video format");
	pthread_mutex_unlock(&pw->lock);
	if (!done)
		goto fail;
	return pw;

fail:
	if (locked)
		api->thread_loop_unlock(pw->loop);
	if (err != NULL && err_len > 0 && err[0] == '\0')
		snprintf(err, (size_t)err_len, "the screen-cast stream could not be set up");
	vshot_pw_close(pw);
	return NULL;
}

int vshot_pw_geometry(VshotPw *pw, int *width, int *height, uint32_t *spa_format, int *is_dmabuf)
{
	if (pw == NULL)
		return -1;

	pthread_mutex_lock(&pw->lock);
	if (!pw->negotiated) {
		pthread_mutex_unlock(&pw->lock);
		return -1;
	}
	*width = (int)pw->width;
	*height = (int)pw->height;
	*spa_format = pw->spa_format;
	*is_dmabuf = pw->dmabuf ? 1 : 0;
	pthread_mutex_unlock(&pw->lock);
	return 0;
}

int vshot_pw_next(VshotPw *pw, VshotPwFrame *out, int timeout_ms)
{
	struct timespec deadline;
	struct pw_buffer *buffer = NULL;
	int result;

	if (pw == NULL || out == NULL)
		return -1;
	memset(out, 0, sizeof(*out));
	out->fd = -1;
	out->kind = VSHOT_PW_KIND_NONE;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&pw->lock);

	while (pw->slot == NULL && !pw->failed && !pw->ended) {
		if (timeout_ms < 0) {
			pthread_cond_wait(&pw->cond, &pw->lock);
			continue;
		}
		if (pthread_cond_timedwait(&pw->cond, &pw->lock, &deadline) == ETIMEDOUT)
			break;
	}

	if (pw->slot != NULL) {
		/* The frame of the previous call goes back only now: the caller's
		 * pixels stay valid — and untouched by the compositor — until the
		 * frame that replaces them is here.  That is what lets the
		 * recording loop send the last frame a second time at the end, to
		 * carry the interval between it and the stop signal, which is the
		 * only way a still screen's last frame gets its true length. */
		if (pw->held != NULL) {
			vshot_pw_recycle_locked(pw, pw->held);
			pw->held = NULL;
		}
		buffer = pw->slot;
		pw->slot = NULL;
		pw->held = buffer;
		vshot_pw_describe(pw, buffer, out);
		result = 0;
	} else if (pw->failed || pw->ended) {
		result = -1;
	} else {
		result = 1;
	}

	pthread_mutex_unlock(&pw->lock);
	return result;
}

const char *vshot_pw_error(VshotPw *pw)
{
	static char fallback[512];

	if (pw == NULL)
		return "the PipeWire client does not exist";

	pthread_mutex_lock(&pw->lock);
	if (pw->error[0] != '\0') {
		pthread_mutex_unlock(&pw->lock);
		return pw->error;
	}
	if (pw->failed || pw->ended) {
		pthread_mutex_unlock(&pw->lock);
		return "the compositor's screen-cast stream ended";
	}
	pthread_mutex_unlock(&pw->lock);

	/* Nothing has gone wrong: the answer is a state, not a failure, and it
	 * says so rather than looking like one. */
	snprintf(fallback, sizeof(fallback), "the screen-cast stream is still connected");
	return fallback;
}

void vshot_pw_close(VshotPw *pw)
{
	if (pw == NULL)
		return;

	if (pw->loop != NULL && pw->loop_started)
		pw->api->thread_loop_stop(pw->loop);

	pthread_mutex_lock(&pw->lock);
	pw->failed = true;
	pw->ended = true;
	vshot_pw_wake(pw);
	pthread_mutex_unlock(&pw->lock);

	/* Buffers the caller still holds go back before the stream goes away,
	 * so the compositor is not left short of them.  The loop is stopped by
	 * now, so nothing else touches the stream. */
	pthread_mutex_lock(&pw->lock);
	vshot_pw_recycle_locked(pw, pw->held);
	vshot_pw_recycle_locked(pw, pw->slot);
	pw->held = NULL;
	pw->slot = NULL;
	pthread_mutex_unlock(&pw->lock);

	if (pw->stream != NULL)
		pw->api->stream_destroy(pw->stream);
	if (pw->core != NULL)
		pw->api->core_disconnect(pw->core);
	if (pw->context != NULL)
		pw->api->context_destroy(pw->context);
	if (pw->loop != NULL)
		pw->api->thread_loop_destroy(pw->loop);

	pthread_mutex_destroy(&pw->lock);
	pthread_cond_destroy(&pw->cond);
	free(pw);
}

/*
 * Loading libpipewire.
 *
 * The handle stays open for the life of the process: unloading it would take
 * the code out from under a thread still inside it, and a recording is not
 * where that should be discovered.  The lookup happens once, under a lock,
 * because several threads can ask first.
 */
static const char *const vshot_pw_libraries[] = {
	"libpipewire-0.3.so.0",
	"libpipewire-0.3.so",
	NULL,
};

static pthread_mutex_t vshot_pw_api_lock = PTHREAD_MUTEX_INITIALIZER;
static void *vshot_pw_handle;
static bool vshot_pw_api_loaded;
static char vshot_pw_api_error[512];

static const struct vshot_pw_api *vshot_pw_api_load(char *error, size_t error_len)
{
	static struct vshot_pw_api api;
	void *symbol;
	size_t index;
	bool gave_up = false;

	pthread_mutex_lock(&vshot_pw_api_lock);

	if (!vshot_pw_api_loaded && vshot_pw_handle == NULL) {
		for (index = 0; vshot_pw_libraries[index] != NULL; index++) {
			vshot_pw_handle = dlopen(vshot_pw_libraries[index], RTLD_NOW | RTLD_LOCAL);
			if (vshot_pw_handle != NULL)
				break;
		}
		if (vshot_pw_handle == NULL) {
			const char *reason = dlerror();

			snprintf(vshot_pw_api_error, sizeof(vshot_pw_api_error),
				 "libpipewire is not installed, and the portal's screen cast travels "
				 "over libpipewire: %s",
				 reason != NULL ? reason : "the shared object could not be opened");
			gave_up = true;
		}
	}

	if (vshot_pw_handle == NULL) {
		if (!gave_up && vshot_pw_api_error[0] == '\0')
			snprintf(vshot_pw_api_error, sizeof(vshot_pw_api_error),
				 "libpipewire could not be loaded");
		pthread_mutex_unlock(&vshot_pw_api_lock);
		if (error != NULL && error_len > 0)
			snprintf(error, error_len, "%s", vshot_pw_api_error);
		return NULL;
	}

	if (!vshot_pw_api_loaded) {
#define VSHOT_PW_SYMBOL(field, symbol_name)                                                        \
	do {                                                                                       \
		symbol = dlsym(vshot_pw_handle, symbol_name);                                      \
		if (symbol == NULL) {                                                              \
			snprintf(vshot_pw_api_error, sizeof(vshot_pw_api_error),                    \
				 "libpipewire is installed but `%s` is missing, so it is too old "  \
				 "for the portal's screen cast",                                    \
				 symbol_name);                                                       \
			pthread_mutex_unlock(&vshot_pw_api_lock);                                  \
			if (error != NULL && error_len > 0)                                        \
				snprintf(error, error_len, "%s", vshot_pw_api_error);              \
			return NULL;                                                               \
		}                                                                                  \
		api.field = (typeof(api.field))symbol;                                             \
	} while (0)

		VSHOT_PW_SYMBOL(init, "pw_init");
		VSHOT_PW_SYMBOL(thread_loop_new, "pw_thread_loop_new");
		VSHOT_PW_SYMBOL(thread_loop_destroy, "pw_thread_loop_destroy");
		VSHOT_PW_SYMBOL(thread_loop_start, "pw_thread_loop_start");
		VSHOT_PW_SYMBOL(thread_loop_stop, "pw_thread_loop_stop");
		VSHOT_PW_SYMBOL(thread_loop_lock, "pw_thread_loop_lock");
		VSHOT_PW_SYMBOL(thread_loop_unlock, "pw_thread_loop_unlock");
		VSHOT_PW_SYMBOL(thread_loop_get_loop, "pw_thread_loop_get_loop");
		VSHOT_PW_SYMBOL(thread_loop_signal, "pw_thread_loop_signal");
		VSHOT_PW_SYMBOL(thread_loop_timed_wait, "pw_thread_loop_timed_wait");
		VSHOT_PW_SYMBOL(context_new, "pw_context_new");
		VSHOT_PW_SYMBOL(context_destroy, "pw_context_destroy");
		VSHOT_PW_SYMBOL(context_connect, "pw_context_connect");
		VSHOT_PW_SYMBOL(context_connect_fd, "pw_context_connect_fd");
		VSHOT_PW_SYMBOL(core_disconnect, "pw_core_disconnect");
		VSHOT_PW_SYMBOL(properties_new, "pw_properties_new");
		VSHOT_PW_SYMBOL(stream_new, "pw_stream_new");
		VSHOT_PW_SYMBOL(stream_destroy, "pw_stream_destroy");
		VSHOT_PW_SYMBOL(stream_connect, "pw_stream_connect");
		VSHOT_PW_SYMBOL(stream_dequeue_buffer, "pw_stream_dequeue_buffer");
		VSHOT_PW_SYMBOL(stream_queue_buffer, "pw_stream_queue_buffer");
		VSHOT_PW_SYMBOL(stream_add_listener, "pw_stream_add_listener");
		VSHOT_PW_SYMBOL(stream_get_state, "pw_stream_get_state");
		VSHOT_PW_SYMBOL(stream_state_as_string, "pw_stream_state_as_string");
#undef VSHOT_PW_SYMBOL

		vshot_pw_api_loaded = true;
	}

	pthread_mutex_unlock(&vshot_pw_api_lock);
	if (error != NULL && error_len > 0)
		error[0] = '\0';
	return &api;
}
