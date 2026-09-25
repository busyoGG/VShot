// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

/*
 * The microphone, as a PipeWire client.
 *
 * `record --mic` records audio beside the video, and the audio comes from the
 * same place the screen cast does: PipeWire.  The microphone is an ordinary
 * source node — WirePlumber's default one — and a capture stream on it
 * delivers buffers at the sample rate the negotiation settles on.  This file
 * is that stream, wrapped in an API small enough for Rust to call.
 *
 * Five things shape it:
 *
 *   * **libpipewire is `dlopen`ed, not linked**, exactly as in
 *     `pipewire_client.c`: a machine without it still builds and still takes
 *     screenshots, and only the paths that need PipeWire say what is missing.
 *
 *   * **Nothing is dropped.**  A screen-cast stream wants the newest frame
 *     and nothing else, so the video client replaces the waiting frame; an
 *     audio stream is a continuous signal, and a dropped buffer is a hole in
 *     the recording.  Every buffer is copied into the ring buffer here, and
 *     the ring only overflows when the *caller* has stopped reading for
 *     seconds — in which case the oldest samples go, not the newest, because
 *     the recording's timeline follows the wall clock.
 *
 *   * **The caller pumps, the thread fills.**  The PipeWire thread loop runs
 *     the protocol and the process callback; the recording loop calls
 *     `vshot_pwa_read` between its frames and feeds what it gets to the
 *     encoder.  One producer, one consumer, one mutex.
 *
 *   * **Before `arm`, samples are dropped.**  The stream is opened before the
 *     video encoder is (its rate and channel count are what the audio encoder
 *     must be opened with), and that is hundreds of milliseconds of audio that
 *     belongs to no recording.  `arm` is the line between "before the
 *     recording" and "the recording", so the first sample of the file is the
 *     first sample after it.
 *
 *   * **Two entry points, one library.**  Besides the recording stream, a
 *     one-shot registry scan lists the session's audio capture sources
 *     (`vshot record mics`), so the CLI and the settings window can offer the
 *     inputs that actually exist rather than a guessed name.  The scan shares
 *     this file's `dlopen` table and thread-loop shape.
 *
 * The format asked for is 48 kHz stereo float, interleaved — PipeWire's own
 * adapters convert from whatever the device natively speaks, which is why a
 * single format can be asked for at all.  A planar float layout is accepted
 * too and interleaved here, because a source that hands one back is not a
 * reason to fail.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>

/* How long `vshot_pwa_open` waits for the format negotiation, in whole
 * seconds per slice; the deadline is checked from the caller's own thread,
 * like the video client does. */
#define VSHOT_PWA_NEGOTIATION_SLICE_SEC 1

/* How many seconds of audio the ring buffer holds.  The recording loop
 * drains it once per frame — every 16 ms at 60 fps — and even the slowest
 * loop (the window recorder's 250 ms poll) leaves this an order of magnitude
 * of headroom. */
#define VSHOT_PWA_RING_SECONDS 4

typedef struct vshot_pwa VshotPwa;

/*
 * The libpipewire entry points, resolved by name.  The same table the video
 * client keeps, minus the fd-based connect: the microphone lives on the
 * user's own PipeWire instance, not on a connection the portal handed over.
 */
struct vshot_pwa_api {
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
	void (*core_disconnect)(struct pw_core *core);
	struct pw_properties *(*properties_new)(const char *key, ...);
	int (*properties_set)(struct pw_properties *properties, const char *key, const char *value);
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
	/* The registry walk `vshot_pwa_list_sources` needs: the node globals
	 * carry what the list shows, so nothing has to be bound. */
	struct pw_registry *(*core_get_registry)(struct pw_core *core, uint32_t version,
						 size_t user_data_size);
	int (*registry_add_listener)(struct pw_registry *registry, struct spa_hook *listener,
				     const struct pw_registry_events *events, void *data);
	int (*core_add_listener)(struct pw_core *core, struct spa_hook *listener,
				 const struct pw_core_events *events, void *data);
	int (*core_sync)(struct pw_core *core, uint32_t id, int seq);
};

/* One client.  Everything the two threads share is under `lock`. */
struct vshot_pwa {
	const struct vshot_pwa_api *api;
	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	/* `pw_stream_add_listener` keeps a *pointer* to this structure for the
	 * life of the stream, so it has to live here rather than on the stack
	 * of the function that connects — the same trap the video client's
	 * comment describes. */
	struct pw_stream_events events;
	bool loop_started;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	/* The negotiation's answer. */
	bool negotiated;
	int rate;
	int channels;
	uint32_t spa_format;

	/* The ring: `ring_frames` frames of `channels` floats, interleaved.
	 * `head` is where the next sample goes, `tail` where the next read
	 * starts; both count frames and wrap at `ring_frames`.  `scratch` is
	 * the interleaving step's room, one ring's worth of frames. */
	float *ring;
	float *scratch;
	size_t ring_frames;
	size_t head;
	size_t tail;
	/* Samples that arrive before `arm` belong to no recording and are
	 * dropped. */
	bool armed;

	bool failed;
	bool ended;
	char error[512];
};

/* Defined with the rest of the entry points; `vshot_pwa_open` unwinds
 * through it, and Rust closes the handle through it. */
void vshot_pwa_close(VshotPwa *pw);
static void vshot_pwa_wake(VshotPwa *pw);

static void vshot_pwa_error_out(char *err, int err_len, const char *format, ...)
{
	va_list arguments;

	if (err == NULL || err_len <= 0)
		return;
	va_start(arguments, format);
	vsnprintf(err, (size_t)err_len, format, arguments);
	va_end(arguments);
}

/* Frames waiting in the ring. */
static size_t vshot_pwa_queued(VshotPwa *pw)
{
	if (pw->ring_frames == 0)
		return 0;
	return (pw->head + pw->ring_frames - pw->tail) % pw->ring_frames;
}

static void vshot_pwa_wake(VshotPwa *pw)
{
	pthread_cond_broadcast(&pw->cond);
	if (pw->loop != NULL)
		pw->api->thread_loop_signal(pw->loop, false);
}

static void vshot_pwa_set_error(VshotPwa *pw, const char *format, ...)
{
	va_list arguments;

	pthread_mutex_lock(&pw->lock);
	va_start(arguments, format);
	vsnprintf(pw->error, sizeof(pw->error), format, arguments);
	va_end(arguments);
	pw->failed = true;
	vshot_pwa_wake(pw);
	pthread_mutex_unlock(&pw->lock);
}

static void vshot_pwa_on_state_changed(void *data, enum pw_stream_state old,
				       enum pw_stream_state state, const char *error)
{
	VshotPwa *pw = data;

	(void)old;
	if (state == PW_STREAM_STATE_ERROR) {
		vshot_pwa_set_error(pw, "the microphone stream failed: %s",
				    error != NULL ? error : "PipeWire did not say why");
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
				 "the microphone stream was closed before it reported an audio "
				 "format");
		vshot_pwa_wake(pw);
		pthread_mutex_unlock(&pw->lock);
	}
}

/* The format is the negotiation's answer: the rate and channel count the
 * encoder has to be opened with. */
static void vshot_pwa_on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	VshotPwa *pw = data;
	struct spa_audio_info_raw info;

	if (id != SPA_PARAM_Format || param == NULL)
		return;
	if (spa_format_audio_raw_parse(param, &info) < 0)
		return;
	if (info.rate == 0 || info.channels == 0)
		return;
	if (info.format != SPA_AUDIO_FORMAT_F32 && info.format != SPA_AUDIO_FORMAT_F32P)
		return;

	pthread_mutex_lock(&pw->lock);
	pw->rate = (int)info.rate;
	pw->channels = (int)info.channels;
	pw->spa_format = (uint32_t)info.format;
	pw->negotiated = true;
	vshot_pwa_wake(pw);
	pthread_mutex_unlock(&pw->lock);
}

/* Writes one buffer's samples into `out` as interleaved floats, at most
 * `want` frames.  A planar layout (F32P) comes in one plane per channel,
 * each `maxsize` bytes apart; an interleaved one arrives as a single plane.
 * Returns the number of frames written. */
static size_t vshot_pwa_interleave(VshotPwa *pw, struct pw_buffer *buffer, float *out,
				   size_t want)
{
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *data = &buf->datas[0];
	size_t channels = (size_t)pw->channels;
	size_t offset;
	size_t count;
	size_t i;
	size_t c;

	if (data->data == NULL || channels == 0)
		return 0;
	offset = SPA_MIN((size_t)data->chunk->offset, (size_t)data->maxsize);
	if (pw->spa_format == SPA_AUDIO_FORMAT_F32P) {
		/* The chunk's size counts the first plane's samples; every
		 * plane carries the same count. */
		count = (size_t)data->chunk->size / sizeof(float);
		if (count > want)
			count = want;
		for (c = 0; c < channels; c++) {
			const float *plane =
				(const float *)((const uint8_t *)data->data +
						(size_t)c * data->maxsize + offset);
			for (i = 0; i < count; i++)
				out[i * channels + c] = plane[i];
		}
		return count;
	}
	count = (size_t)data->chunk->size / (sizeof(float) * channels);
	if (count > want)
		count = want;
	memcpy(out, (const uint8_t *)data->data + offset, count * channels * sizeof(float));
	return count;
}

/* A buffer is ready.  Copying it here, in the source's own callback, is what
 * keeps a buffer from being recycled while its samples are still on their way
 * to the encoder; the copy is a few kilobytes. */
static void vshot_pwa_on_process(void *data)
{
	VshotPwa *pw = data;
	struct pw_buffer *buffer;
	size_t frames;
	size_t available;
	size_t room;
	size_t first;
	size_t second;

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
	if (!pw->armed || pw->channels <= 0 || pw->rate <= 0) {
		/* Before the recording: this audio belongs to nobody. */
		pw->api->stream_queue_buffer(pw->stream, buffer);
		pthread_mutex_unlock(&pw->lock);
		return;
	}
	if (pw->ring == NULL) {
		/* The ring is allocated when the first kept samples arrive, so
		 * that a negotiation failure allocates nothing. */
		pw->ring_frames = (size_t)pw->rate * VSHOT_PWA_RING_SECONDS;
		pw->ring = malloc(pw->ring_frames * (size_t)pw->channels * sizeof(float));
		pw->scratch = malloc(pw->ring_frames * (size_t)pw->channels * sizeof(float));
		if (pw->ring == NULL || pw->scratch == NULL) {
			free(pw->ring);
			free(pw->scratch);
			pw->ring = NULL;
			pw->scratch = NULL;
			pw->ring_frames = 0;
			vshot_pwa_set_error(pw, "out of memory for the microphone ring buffer");
			pw->api->stream_queue_buffer(pw->stream, buffer);
			pthread_mutex_unlock(&pw->lock);
			return;
		}
		pw->head = 0;
		pw->tail = 0;
	}
	/* One buffer's worth of frames, interleaved into the scratch: the
	 * chunk's own size is the count, since the data's maxsize can be
	 * padded. */
	frames = vshot_pwa_interleave(pw, buffer, pw->scratch, pw->ring_frames);
	if (frames > pw->ring_frames)
		frames = pw->ring_frames;
	available = vshot_pwa_queued(pw);
	room = pw->ring_frames - available;
	if (frames > room) {
		/* The caller has not read for a while.  The oldest samples go:
		 * the recording follows the wall clock, so the newest audio is
		 * the audio that is still true. */
		size_t drop = frames - room;
		if (drop > available)
			drop = available;
		pw->tail = (pw->tail + drop) % pw->ring_frames;
		room = pw->ring_frames - vshot_pwa_queued(pw);
	}
	if (frames > room)
		frames = room;
	if (frames > 0) {
		size_t channels = (size_t)pw->channels;
		first = pw->head + frames <= pw->ring_frames ? frames : pw->ring_frames - pw->head;
		second = frames - first;
		memcpy(pw->ring + pw->head * channels, pw->scratch, first * channels * sizeof(float));
		if (second > 0)
			memcpy(pw->ring, pw->scratch + first * channels,
			       second * channels * sizeof(float));
		pw->head = (pw->head + frames) % pw->ring_frames;
	}
	pw->api->stream_queue_buffer(pw->stream, buffer);
	pthread_mutex_unlock(&pw->lock);
}

/* The format this client asks for: 48 kHz stereo float, interleaved.  The
 * source side's adapters convert from whatever the device natively speaks,
 * which is why one format can be asked for at all. */
static struct spa_pod *vshot_pwa_build_format(struct spa_pod_builder *builder)
{
	struct spa_pod_frame object;
	struct spa_audio_info_raw info;

	/* The three fields that matter: float samples, two channels, 48 kHz.
	 * The channel positions stay unset, which a source reads as the plain
	 * stereo layout. */
	memset(&info, 0, sizeof(info));
	info.format = SPA_AUDIO_FORMAT_F32;
	info.channels = 2;
	info.rate = 48000;
	spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio), 0);
	spa_pod_builder_add(builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	spa_format_audio_raw_build(builder, SPA_PARAM_Format, &info);
	return spa_pod_builder_pop(builder, &object);
}

/* ---------------------------------------------------------------------------
 * Loading libpipewire
 * ---------------------------------------------------------------------------
 *
 * The handle stays open for the life of the process, as in the video client:
 * unloading it would take the code out from under a thread still inside it.
 * The lookup happens once, under a lock, because several threads can ask.
 */

static const char *const vshot_pwa_libraries[] = {
	"libpipewire-0.3.so.0",
	"libpipewire-0.3.so",
	NULL,
};

static pthread_mutex_t vshot_pwa_api_lock = PTHREAD_MUTEX_INITIALIZER;
static void *vshot_pwa_handle;
static bool vshot_pwa_api_loaded;
static char vshot_pwa_api_error[512];

static const struct vshot_pwa_api *vshot_pwa_api_load(char *error, size_t error_len)
{
	static struct vshot_pwa_api api;
	void *symbol;
	size_t index;

	pthread_mutex_lock(&vshot_pwa_api_lock);

	if (!vshot_pwa_api_loaded && vshot_pwa_handle == NULL) {
		for (index = 0; vshot_pwa_libraries[index] != NULL; index++) {
			vshot_pwa_handle =
				dlopen(vshot_pwa_libraries[index], RTLD_NOW | RTLD_LOCAL);
			if (vshot_pwa_handle != NULL)
				break;
		}
		if (vshot_pwa_handle == NULL) {
			const char *reason = dlerror();

			snprintf(vshot_pwa_api_error, sizeof(vshot_pwa_api_error),
				 "libpipewire is not installed, and the microphone is read "
				 "through it: %s",
				 reason != NULL ? reason : "the shared object could not be opened");
		}
	}

	if (vshot_pwa_handle == NULL) {
		pthread_mutex_unlock(&vshot_pwa_api_lock);
		if (error != NULL && error_len > 0)
			snprintf(error, error_len, "%s", vshot_pwa_api_error);
		return NULL;
	}

	if (!vshot_pwa_api_loaded) {
#define VSHOT_PWA_SYMBOL(field, symbol_name)                                                    \
	do {                                                                                    \
		symbol = dlsym(vshot_pwa_handle, symbol_name);                                  \
		if (symbol == NULL) {                                                           \
			snprintf(vshot_pwa_api_error, sizeof(vshot_pwa_api_error),              \
				 "libpipewire is installed but `%s` is missing, so it is too "  \
				 "old for the microphone",                                      \
				 symbol_name);                                                  \
			pthread_mutex_unlock(&vshot_pwa_api_lock);                              \
			if (error != NULL && error_len > 0)                                     \
				snprintf(error, error_len, "%s", vshot_pwa_api_error);          \
			return NULL;                                                            \
		}                                                                               \
		api.field = (typeof(api.field))symbol;                                          \
	} while (0)

		VSHOT_PWA_SYMBOL(init, "pw_init");
		VSHOT_PWA_SYMBOL(thread_loop_new, "pw_thread_loop_new");
		VSHOT_PWA_SYMBOL(thread_loop_destroy, "pw_thread_loop_destroy");
		VSHOT_PWA_SYMBOL(thread_loop_start, "pw_thread_loop_start");
		VSHOT_PWA_SYMBOL(thread_loop_stop, "pw_thread_loop_stop");
		VSHOT_PWA_SYMBOL(thread_loop_lock, "pw_thread_loop_lock");
		VSHOT_PWA_SYMBOL(thread_loop_unlock, "pw_thread_loop_unlock");
		VSHOT_PWA_SYMBOL(thread_loop_get_loop, "pw_thread_loop_get_loop");
		VSHOT_PWA_SYMBOL(thread_loop_signal, "pw_thread_loop_signal");
		VSHOT_PWA_SYMBOL(thread_loop_timed_wait, "pw_thread_loop_timed_wait");
		VSHOT_PWA_SYMBOL(context_new, "pw_context_new");
		VSHOT_PWA_SYMBOL(context_destroy, "pw_context_destroy");
		VSHOT_PWA_SYMBOL(context_connect, "pw_context_connect");
		VSHOT_PWA_SYMBOL(core_disconnect, "pw_core_disconnect");
		VSHOT_PWA_SYMBOL(properties_new, "pw_properties_new");
		VSHOT_PWA_SYMBOL(properties_set, "pw_properties_set");
		VSHOT_PWA_SYMBOL(stream_new, "pw_stream_new");
		VSHOT_PWA_SYMBOL(stream_destroy, "pw_stream_destroy");
		VSHOT_PWA_SYMBOL(stream_connect, "pw_stream_connect");
		VSHOT_PWA_SYMBOL(stream_dequeue_buffer, "pw_stream_dequeue_buffer");
		VSHOT_PWA_SYMBOL(stream_queue_buffer, "pw_stream_queue_buffer");
		VSHOT_PWA_SYMBOL(stream_add_listener, "pw_stream_add_listener");
		VSHOT_PWA_SYMBOL(stream_get_state, "pw_stream_get_state");
		VSHOT_PWA_SYMBOL(stream_state_as_string, "pw_stream_state_as_string");
		VSHOT_PWA_SYMBOL(core_get_registry, "pw_core_get_registry");
		VSHOT_PWA_SYMBOL(registry_add_listener, "pw_registry_add_listener");
		VSHOT_PWA_SYMBOL(core_add_listener, "pw_core_add_listener");
		VSHOT_PWA_SYMBOL(core_sync, "pw_core_sync");
#undef VSHOT_PWA_SYMBOL

		vshot_pwa_api_loaded = true;
	}

	pthread_mutex_unlock(&vshot_pwa_api_lock);
	if (error != NULL && error_len > 0)
		error[0] = '\0';
	return &api;
}

/* ---------------------------------------------------------------------------
 * The entry points, one for one with the declarations in `pipewire_audio.rs`.
 * ---------------------------------------------------------------------------
 */

int vshot_pwa_available(void)
{
	return vshot_pwa_api_load(NULL, 0) != NULL ? 1 : 0;
}

const char *vshot_pwa_load_error(void)
{
	static char error[512];

	if (vshot_pwa_api_load(error, sizeof(error)) != NULL)
		error[0] = '\0';
	else if (error[0] == '\0')
		snprintf(error, sizeof(error), "libpipewire could not be loaded");
	return error;
}

/*
 * Opens the microphone.  `target` is a node name or serial to record from,
 * or NULL for the default source; `timeout_ms` bounds the negotiation, which
 * is a handful of round trips on the local socket.
 */
VshotPwa *vshot_pwa_open(const char *target, int timeout_ms, char *err, int err_len)
{
	const struct vshot_pwa_api *api = vshot_pwa_api_load(err, (size_t)err_len);
	struct spa_pod_builder builder;
	struct spa_pod *params[1];
	uint8_t storage[1024];
	struct pw_properties *properties;
	struct timespec deadline;
	bool locked = false;
	bool done;
	VshotPwa *pw;

	if (err != NULL && err_len > 0)
		err[0] = '\0';
	if (api == NULL)
		return NULL;
	if (timeout_ms <= 0)
		timeout_ms = 15000;

	pw = (VshotPwa *)calloc(1, sizeof(*pw));
	if (pw == NULL) {
		vshot_pwa_error_out(err, err_len, "out of memory for the microphone client");
		return NULL;
	}
	pw->api = api;
	pthread_mutex_init(&pw->lock, NULL);
	pthread_cond_init(&pw->cond, NULL);

	api->init(NULL, NULL);

	pw->loop = api->thread_loop_new("vshot-mic", NULL);
	if (pw->loop == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire thread loop could not be created");
		goto fail;
	}

	api->thread_loop_lock(pw->loop);
	locked = true;

	/* The context's own properties: a name, and nothing else.  The stream's
	 * are the ones a policy daemon reads (below), because that is where
	 * `media.type` / `media.category` belong — WirePlumber picks the default
	 * source for a capture stream from them. */
	properties = api->properties_new(PW_KEY_APP_NAME, "vshot", NULL);
	pw->context = api->context_new(api->thread_loop_get_loop(pw->loop), properties, 0);
	if (pw->context == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire context could not be created: %s",
				    strerror(errno));
		goto fail;
	}

	pw->core = api->context_connect(pw->context, NULL, 0);
	if (pw->core == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire session could not be reached: %s",
				    strerror(errno));
		goto fail;
	}

	/* What the stream is: an audio capture stream (the microphone).  The
	 * media type and category are what WirePlumber's policy reads to decide
	 * which node a stream with no explicit target gets; without them a
	 * capture stream has nothing to match and the connection comes back
	 * "no target node available".  `target` names a node or serial
	 * explicitly, which overrides the policy's choice. */
	properties = api->properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
					 "Capture", PW_KEY_MEDIA_ROLE, "Production", NULL);
	if (target != NULL && target[0] != '\0')
		api->properties_set(properties, PW_KEY_TARGET_OBJECT, target);
	pw->stream = api->stream_new(pw->core, "vshot-mic", properties);
	if (pw->stream == NULL) {
		vshot_pwa_error_out(err, err_len, "the microphone stream could not be created: %s",
				    strerror(errno));
		goto fail;
	}

	memset(&pw->events, 0, sizeof(pw->events));
	pw->events.version = PW_VERSION_STREAM_EVENTS;
	pw->events.state_changed = vshot_pwa_on_state_changed;
	pw->events.param_changed = vshot_pwa_on_param_changed;
	pw->events.process = vshot_pwa_on_process;
	api->stream_add_listener(pw->stream, &pw->stream_listener, &pw->events, pw);

	spa_pod_builder_init(&builder, storage, sizeof(storage));
	params[0] = vshot_pwa_build_format(&builder);

	if (api->stream_connect(pw->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
					PW_STREAM_FLAG_RT_PROCESS,
				(const struct spa_pod *const *)params, 1) < 0) {
		vshot_pwa_error_out(err, err_len, "the microphone could not be connected: %s",
				    strerror(errno));
		goto fail;
	}

	api->thread_loop_unlock(pw->loop);
	locked = false;

	if (api->thread_loop_start(pw->loop) < 0) {
		vshot_pwa_error_out(err, err_len, "the PipeWire thread loop could not be started: %s",
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
			vshot_pwa_error_out(err, err_len,
					    "the microphone did not report an audio format "
					    "within %d ms",
					    timeout_ms);
			goto fail;
		}

		api->thread_loop_lock(pw->loop);
		api->thread_loop_timed_wait(pw->loop, VSHOT_PWA_NEGOTIATION_SLICE_SEC);
		api->thread_loop_unlock(pw->loop);
	}

	pthread_mutex_lock(&pw->lock);
	done = pw->negotiated && pw->rate > 0 && pw->channels > 0;
	if (!done && err != NULL && err_len > 0)
		snprintf(err, (size_t)err_len, "%s",
			 pw->error[0] != '\0' ? pw->error
					      : "the microphone stream ended before it reported an "
						"audio format");
	pthread_mutex_unlock(&pw->lock);
	if (!done)
		goto fail;
	return pw;

fail:
	if (locked)
		api->thread_loop_unlock(pw->loop);
	if (err != NULL && err_len > 0 && err[0] == '\0')
		snprintf(err, (size_t)err_len, "the microphone stream could not be set up");
	vshot_pwa_close(pw);
	return NULL;
}

int vshot_pwa_geometry(VshotPwa *pw, int *rate, int *channels)
{
	if (pw == NULL)
		return -1;

	pthread_mutex_lock(&pw->lock);
	if (!pw->negotiated) {
		pthread_mutex_unlock(&pw->lock);
		return -1;
	}
	*rate = pw->rate;
	*channels = pw->channels;
	pthread_mutex_unlock(&pw->lock);
	return 0;
}

/* The line between "before the recording" and "the recording": samples from
 * now on are kept.  Anything already in the ring goes, so the first sample
 * the caller reads is the first one after this call. */
void vshot_pwa_arm(VshotPwa *pw)
{
	if (pw == NULL)
		return;
	pthread_mutex_lock(&pw->lock);
	pw->tail = pw->head;
	pw->armed = true;
	pthread_mutex_unlock(&pw->lock);
}

/* Reads up to `frames` frames into `out`, which must have room for
 * `frames * channels` floats.  Returns the count read (0 is "nothing yet",
 * not a failure), or -1 when the stream has ended. */
int vshot_pwa_read(VshotPwa *pw, float *out, int frames)
{
	size_t channels;
	size_t available;
	size_t first;
	size_t second;

	if (pw == NULL || out == NULL || frames <= 0)
		return -1;

	pthread_mutex_lock(&pw->lock);
	if (pw->failed)
		goto ended;
	channels = (size_t)pw->channels;
	available = vshot_pwa_queued(pw);
	if (available == 0) {
		pthread_mutex_unlock(&pw->lock);
		return pw->ended ? -1 : 0;
	}
	if ((size_t)frames > available)
		frames = (int)available;
	first = pw->tail + (size_t)frames <= pw->ring_frames ? (size_t)frames
							    : pw->ring_frames - pw->tail;
	second = (size_t)frames - first;
	memcpy(out, pw->ring + pw->tail * channels, first * channels * sizeof(float));
	if (second > 0)
		memcpy(out + first * channels, pw->ring, second * channels * sizeof(float));
	pw->tail = (pw->tail + (size_t)frames) % pw->ring_frames;
	pthread_mutex_unlock(&pw->lock);
	return frames;

ended:
	pthread_mutex_unlock(&pw->lock);
	return -1;
}

const char *vshot_pwa_error(VshotPwa *pw)
{
	static char fallback[512];

	if (pw == NULL)
		return "the microphone client does not exist";

	pthread_mutex_lock(&pw->lock);
	if (pw->error[0] != '\0') {
		pthread_mutex_unlock(&pw->lock);
		return pw->error;
	}
	if (pw->failed || pw->ended) {
		pthread_mutex_unlock(&pw->lock);
		return "the microphone stream ended";
	}
	pthread_mutex_unlock(&pw->lock);

	/* Nothing has gone wrong: the answer is a state, not a failure, and it
	 * says so rather than looking like one. */
	snprintf(fallback, sizeof(fallback), "the microphone stream is still connected");
	return fallback;
}

void vshot_pwa_close(VshotPwa *pw)
{
	if (pw == NULL)
		return;

	if (pw->loop != NULL && pw->loop_started)
		pw->api->thread_loop_stop(pw->loop);

	pthread_mutex_lock(&pw->lock);
	pw->failed = true;
	pw->ended = true;
	vshot_pwa_wake(pw);
	pthread_mutex_unlock(&pw->lock);

	if (pw->stream != NULL)
		pw->api->stream_destroy(pw->stream);
	if (pw->core != NULL)
		pw->api->core_disconnect(pw->core);
	if (pw->context != NULL)
		pw->api->context_destroy(pw->context);
	if (pw->loop != NULL)
		pw->api->thread_loop_destroy(pw->loop);

	free(pw->ring);
	free(pw->scratch);
	pthread_mutex_destroy(&pw->lock);
	pthread_cond_destroy(&pw->cond);
	free(pw);
}

/* ---------------------------------------------------------------------------
 * Listing the session's capture sources (`vshot record mics`)
 * ---------------------------------------------------------------------------
 *
 * A one-shot registry scan: connect, walk every `Audio/Source*` node global,
 * wait for the core's `done` event, and write the list out as tab-separated
 * lines — `serial \t name \t description`, one per source.  The registry
 * globals carry those properties themselves, so nothing is bound and the scan
 * finishes in one round trip.  Tab and newline inside a field would break the
 * format, so they are replaced with spaces; a field is cut at a whole UTF-8
 * sequence.
 */

/* How long the scan waits for the core's answer when the caller does not say.
 * A local socket round trip is milliseconds; the deadline is only there so a
 * wedged session cannot hang the CLI. */
#define VSHOT_PWA_LIST_DEFAULT_TIMEOUT_MS 5000

/* How many nodes/clients the per-application scan remembers.  A desktop with
 * more playing applications than this is one the lookup was never going to
 * pick the right one out of anyway, and the oldest entries are simply not
 * candidates. */
#define VSHOT_PWA_MAX_APP_NODES 128

typedef struct vshot_pwa_list {
	const struct vshot_pwa_api *api;
	struct pw_thread_loop *loop;
	bool started;
	pthread_mutex_t lock;
	bool done;
	bool failed;
	bool sync_sent;
	char error[512];
	char *out;
	int out_len;
	int used;
	int count;
	int sync_seq;
	/* Two modes share this scan: the source list (`mode` 0) writes every
	 * Audio/Source; the per-application lookup (`mode` 1) writes the serial of
	 * one application's playback node.
	 *
	 * The application lookup is a two-pass scan over the same registry walk,
	 * because the pid is not on the node.  A node's own properties carry
	 * `client.id`, and the *client* object — a separate global — is the one
	 * that carries `application.process.id`.  So the pass records, for every
	 * `Stream/Output/Audio` node, its serial and its `client.id`, and for
	 * every client its pid; when the walk finishes the node whose client has
	 * the wanted pid is picked.  Both tables are small (tens of entries) and
	 * the scan is one round trip either way. */
	int mode;
	char pid[32];
	/* Node id -> serial and client id, as text (the registry reports them as
	 * strings in the properties). */
	struct {
		char node_id[24];
		char serial[24];
		char client_id[24];
	} nodes[VSHOT_PWA_MAX_APP_NODES];
	int node_count;
	/* Client id -> pid, as text. */
	struct {
		char id[24];
		char pid[24];
	} clients[VSHOT_PWA_MAX_APP_NODES];
	int client_count;
	/* Set by the per-application scan when a node matched: the serial, which
	 * is what `vshot_pwa_open`'s target wants. */
	char found[32];
	bool found_any;
} VshotPwaList;

#define VSHOT_PWA_LIST_SOURCES 0
#define VSHOT_PWA_LIST_APP 1

/* Cuts a byte string back to the last whole UTF-8 sequence.  Only called on a
 * field that hit its cap; without it a truncated multi-byte character would
 * end the line in the middle of a sequence and the reader would show a
 * replacement glyph. */
static void vshot_pwa_list_trim_utf8(char *text, size_t *length)
{
	size_t n = *length;

	while (n > 0 && ((unsigned char)text[n - 1] & 0xC0) == 0x80)
		n--;
	if (n > 0 && ((unsigned char)text[n - 1] & 0xC0) == 0xC0)
		n--;
	*length = n;
}

/* Copies `src` into `dst` (room including the NUL), replacing the bytes that
 * would break the line format.  Returns the length written. */
static size_t vshot_pwa_list_field(char *dst, size_t room, const char *src)
{
	size_t n = 0;

	if (src != NULL) {
		for (; src[n] != '\0' && n + 1 < room; n++) {
			const char c = src[n];
			dst[n] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
		}
	}
	dst[n] = '\0';
	if (src != NULL && src[n] != '\0')
		vshot_pwa_list_trim_utf8(dst, &n);
	return n;
}

static void vshot_pwa_list_global(void *data, uint32_t id, uint32_t permissions, const char *type,
				  uint32_t version, const struct spa_dict *props)
{
	VshotPwaList *list = data;
	const char *media_class;
	const char *name;
	const char *description;
	const char *serial;
	char line[1280];
	char field[512];
	size_t length;
	int written;
	int room;

	(void)permissions;
	(void)version;
	if (props == NULL || type == NULL)
		return;
	if (list->mode == VSHOT_PWA_LIST_APP) {
		/* The application scan records the two halves of the mapping and
		 * resolves it after the walk (see `vshot_pwa_find_app_node`): a
		 * Stream/Output node contributes its serial and its client id, a
		 * Client contributes its pid.  The pid is not on the node — a node's
		 * `application.process.id` is unset — which is why this is two
		 * passes and not one.
		 *
		 * A global's id is the callback's `id` argument; it is not repeated as
		 * an `object.id` property on a client global (it is on a node's bound
		 * info, but not in the registry's global props), which is why the
		 * client's own id is taken from the argument and the node's
		 * `client.id` is compared against it. */
		if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
			const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
			const char *serial;
			const char *client_id;
			if (media_class == NULL || strncmp(media_class, "Stream/Output", 13) != 0)
				return;
			serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
			client_id = spa_dict_lookup(props, PW_KEY_CLIENT_ID);
			if (serial == NULL || client_id == NULL)
				return;
			pthread_mutex_lock(&list->lock);
			if (list->node_count < VSHOT_PWA_MAX_APP_NODES) {
				int i = list->node_count;
				snprintf(list->nodes[i].node_id, sizeof(list->nodes[i].node_id), "%u",
					 id);
				snprintf(list->nodes[i].serial, sizeof(list->nodes[i].serial), "%s",
					 serial);
				snprintf(list->nodes[i].client_id, sizeof(list->nodes[i].client_id), "%s",
					 client_id);
				list->node_count++;
			}
			pthread_mutex_unlock(&list->lock);
			return;
		}
		if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
			/* The registry's global for a client carries `pipewire.sec.pid`
			 * (the process the protocol connection came from) but *not*
			 * `application.process.id` — that one is set on the client's
			 * bound info, which a registry walk never sees.  `sec.pid` is the
			 * same process id, and it is the one a window recording has. */
			const char *pid = spa_dict_lookup(props, PW_KEY_SEC_PID);
			if (pid == NULL)
				return;
			pthread_mutex_lock(&list->lock);
			if (list->client_count < VSHOT_PWA_MAX_APP_NODES) {
				int i = list->client_count;
				snprintf(list->clients[i].id, sizeof(list->clients[i].id), "%u", id);
				snprintf(list->clients[i].pid, sizeof(list->clients[i].pid), "%s", pid);
				list->client_count++;
			}
			pthread_mutex_unlock(&list->lock);
			return;
		}
		return;
	}
	if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;
	media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (media_class == NULL || strncmp(media_class, "Audio/Source", 12) != 0)
		return;

	name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
	serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
	/* An input device always has a name and usually a description; the
	 * short nick is the fallback for the rare node that has only one, so a
	 * listed entry always carries something readable. */
	if (description == NULL)
		description = spa_dict_lookup(props, PW_KEY_NODE_NICK);
	if (name == NULL && description == NULL)
		return;
	if (name == NULL)
		name = "";
	if (description == NULL)
		description = "";
	if (serial == NULL)
		serial = "";

	written = 0;
	vshot_pwa_list_field(field, sizeof(field), serial);
	written += snprintf(line + written, sizeof(line) - (size_t)written, "%s\t", field);
	vshot_pwa_list_field(field, sizeof(field), name);
	written += snprintf(line + written, sizeof(line) - (size_t)written, "%s\t", field);
	vshot_pwa_list_field(field, sizeof(field), description);
	written += snprintf(line + written, sizeof(line) - (size_t)written, "%s\n", field);

	length = (size_t)written;
	pthread_mutex_lock(&list->lock);
	room = list->out_len - 1 - list->used;
	if ((int)length <= room) {
		memcpy(list->out + list->used, line, length);
		list->used += (int)length;
		list->count++;
	}
	pthread_mutex_unlock(&list->lock);
}

static void vshot_pwa_list_done(void *data, uint32_t id, int seq)
{
	VshotPwaList *list = data;

	if (id != PW_ID_CORE)
		return;
	pthread_mutex_lock(&list->lock);
	if (list->sync_sent && seq == list->sync_seq)
		list->done = true;
	pthread_mutex_unlock(&list->lock);
}

static void vshot_pwa_list_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	VshotPwaList *list = data;

	(void)id;
	(void)seq;
	pthread_mutex_lock(&list->lock);
	if (list->error[0] == '\0') {
		snprintf(list->error, sizeof(list->error), "the PipeWire session reported an error: %s",
			 message != NULL ? message : strerror(-res));
	}
	list->failed = true;
	pthread_mutex_unlock(&list->lock);
}

/* Lists the session's capture sources into `out` as tab-separated lines:
 * `serial \t name \t description`, one per source.  Returns the number of
 * sources written (0 is an empty session, not a failure), or -1 with `err`
 * filled in.  The text is always NUL-terminated, and a line that would not
 * fit is left out entirely rather than cut. */
int vshot_pwa_list_sources(char *out, int out_len, int timeout_ms, char *err, int err_len)
{
	static const struct pw_registry_events registry_events = {
		PW_VERSION_REGISTRY_EVENTS,
		.global = vshot_pwa_list_global,
	};
	static const struct pw_core_events core_events = {
		PW_VERSION_CORE_EVENTS,
		.done = vshot_pwa_list_done,
		.error = vshot_pwa_list_error,
	};
	const struct vshot_pwa_api *api = vshot_pwa_api_load(err, (size_t)err_len);
	VshotPwaList list;
	struct pw_context *context = NULL;
	struct pw_core *core = NULL;
	struct pw_registry *registry = NULL;
	struct pw_properties *context_properties = NULL;
	struct spa_hook registry_listener;
	struct spa_hook core_listener;
	struct timespec deadline;
	bool locked = false;
	int result = -1;

	if (err != NULL && err_len > 0)
		err[0] = '\0';
	if (api == NULL)
		return -1;
	if (out == NULL || out_len <= 0) {
		vshot_pwa_error_out(err, err_len, "the list needs a buffer to be written into");
		return -1;
	}
	if (timeout_ms <= 0)
		timeout_ms = VSHOT_PWA_LIST_DEFAULT_TIMEOUT_MS;

	memset(&list, 0, sizeof(list));
	list.api = api;
	list.out = out;
	list.out_len = out_len;
	list.used = 0;
	list.count = 0;
	list.mode = VSHOT_PWA_LIST_SOURCES;
	pthread_mutex_init(&list.lock, NULL);
	out[0] = '\0';

	api->init(NULL, NULL);

	list.loop = api->thread_loop_new("vshot-mics", NULL);
	if (list.loop == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire thread loop could not be created");
		goto out;
	}

	api->thread_loop_lock(list.loop);
	locked = true;

	context_properties = api->properties_new(PW_KEY_APP_NAME, "vshot", NULL);
	context = api->context_new(api->thread_loop_get_loop(list.loop), context_properties, 0);
	context_properties = NULL;
	if (context == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire context could not be created: %s",
				    strerror(errno));
		goto out;
	}
	core = api->context_connect(context, NULL, 0);
	if (core == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire session could not be reached: %s",
				    strerror(errno));
		goto out;
	}

	memset(&registry_listener, 0, sizeof(registry_listener));
	memset(&core_listener, 0, sizeof(core_listener));
	registry = api->core_get_registry(core, PW_VERSION_REGISTRY, 0);
	if (registry == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire registry could not be opened");
		goto out;
	}
	api->registry_add_listener(registry, &registry_listener, &registry_events, &list);
	api->core_add_listener(core, &core_listener, &core_events, &list);

	pthread_mutex_lock(&list.lock);
	list.sync_seq = api->core_sync(core, PW_ID_CORE, 0);
	list.sync_sent = true;
	pthread_mutex_unlock(&list.lock);

	api->thread_loop_unlock(list.loop);
	locked = false;

	if (api->thread_loop_start(list.loop) < 0) {
		vshot_pwa_error_out(err, err_len, "the PipeWire thread loop could not be started: %s",
				    strerror(errno));
		goto out;
	}
	list.started = true;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	for (;;) {
		struct timespec now;
		bool done;

		pthread_mutex_lock(&list.lock);
		done = list.done || list.failed;
		pthread_mutex_unlock(&list.lock);
		if (done)
			break;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
			vshot_pwa_error_out(err, err_len,
					    "the PipeWire session did not answer within %d ms",
					    timeout_ms);
			goto out;
		}

		api->thread_loop_lock(list.loop);
		api->thread_loop_timed_wait(list.loop, VSHOT_PWA_NEGOTIATION_SLICE_SEC);
		api->thread_loop_unlock(list.loop);
	}

	pthread_mutex_lock(&list.lock);
	result = list.count;
	if (list.failed && result == 0 && err != NULL && err_len > 0)
		snprintf(err, (size_t)err_len, "%s",
			 list.error[0] != '\0' ? list.error : "the source list could not be read");
	pthread_mutex_unlock(&list.lock);

out:
	if (locked)
		api->thread_loop_unlock(list.loop);
	/* The order `vshot_pwa_close` uses, and for the same reason: the
	 * context's objects hold sources in the loop, so they have to go before
	 * the loop is destroyed — destroying the loop first leaves the context
	 * removing sources from a dead loop and libpipewire aborts. */
	if (list.loop != NULL && list.started)
		api->thread_loop_stop(list.loop);
	if (core != NULL)
		api->core_disconnect(core);
	if (context != NULL)
		api->context_destroy(context);
	if (list.loop != NULL)
		api->thread_loop_destroy(list.loop);
	pthread_mutex_destroy(&list.lock);
	if (list.used < out_len)
		out[list.used] = '\0';
	return result;
}

/* Finds the PipeWire node one application is playing into and writes its
 * serial into `out` (room `out_len`): the node a capture stream binds to in
 * order to record that application's audio and nothing else.
 *
 * The match is `application.process.id` == `pid` on a `Stream/Output/Audio`
 * node — the pid of the process that opened the playback stream, which is the
 * pid a window recording already has for the window it is recording.  The
 * serial is written rather than the node id because that is what
 * `PW_KEY_TARGET_OBJECT` accepts and it survives the node being recreated
 * between the lookup and the connect.
 *
 * Returns 1 when a node was found (and `out` holds its serial), 0 when the
 * application is not playing anything (not a failure — the recording simply
 * has no application audio yet), or -1 with `err` filled in when the session
 * could not be read at all. */
int vshot_pwa_find_app_node(const char *pid, char *out, int out_len, int timeout_ms, char *err,
			    int err_len)
{
	static const struct pw_registry_events registry_events = {
		PW_VERSION_REGISTRY_EVENTS,
		.global = vshot_pwa_list_global,
	};
	static const struct pw_core_events core_events = {
		PW_VERSION_CORE_EVENTS,
		.done = vshot_pwa_list_done,
		.error = vshot_pwa_list_error,
	};
	const struct vshot_pwa_api *api = vshot_pwa_api_load(err, (size_t)err_len);
	VshotPwaList list;
	struct pw_context *context = NULL;
	struct pw_core *core = NULL;
	struct pw_registry *registry = NULL;
	struct pw_properties *context_properties = NULL;
	struct spa_hook registry_listener;
	struct spa_hook core_listener;
	struct timespec deadline;
	bool locked = false;
	int result = -1;

	if (err != NULL && err_len > 0)
		err[0] = '\0';
	if (out != NULL && out_len > 0)
		out[0] = '\0';
	if (api == NULL)
		return -1;
	if (pid == NULL || pid[0] == '\0' || out == NULL || out_len <= 0) {
		vshot_pwa_error_out(err, err_len, "the application lookup needs a pid and a buffer");
		return -1;
	}
	if (timeout_ms <= 0)
		timeout_ms = VSHOT_PWA_LIST_DEFAULT_TIMEOUT_MS;

	memset(&list, 0, sizeof(list));
	list.api = api;
	list.mode = VSHOT_PWA_LIST_APP;
	snprintf(list.pid, sizeof(list.pid), "%s", pid);
	pthread_mutex_init(&list.lock, NULL);

	api->init(NULL, NULL);

	list.loop = api->thread_loop_new("vshot-appaudio", NULL);
	if (list.loop == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire thread loop could not be created");
		goto out;
	}

	api->thread_loop_lock(list.loop);
	locked = true;

	context_properties = api->properties_new(PW_KEY_APP_NAME, "vshot", NULL);
	context = api->context_new(api->thread_loop_get_loop(list.loop), context_properties, 0);
	context_properties = NULL;
	if (context == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire context could not be created: %s",
				    strerror(errno));
		goto out;
	}
	core = api->context_connect(context, NULL, 0);
	if (core == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire session could not be reached: %s",
				    strerror(errno));
		goto out;
	}

	memset(&registry_listener, 0, sizeof(registry_listener));
	memset(&core_listener, 0, sizeof(core_listener));
	registry = api->core_get_registry(core, PW_VERSION_REGISTRY, 0);
	if (registry == NULL) {
		vshot_pwa_error_out(err, err_len, "the PipeWire registry could not be opened");
		goto out;
	}
	api->registry_add_listener(registry, &registry_listener, &registry_events, &list);
	api->core_add_listener(core, &core_listener, &core_events, &list);

	pthread_mutex_lock(&list.lock);
	list.sync_seq = api->core_sync(core, PW_ID_CORE, 0);
	list.sync_sent = true;
	pthread_mutex_unlock(&list.lock);

	api->thread_loop_unlock(list.loop);
	locked = false;

	if (api->thread_loop_start(list.loop) < 0) {
		vshot_pwa_error_out(err, err_len, "the PipeWire thread loop could not be started: %s",
				    strerror(errno));
		goto out;
	}
	list.started = true;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	for (;;) {
		struct timespec now;
		bool done;

		pthread_mutex_lock(&list.lock);
		done = list.done || list.failed;
		pthread_mutex_unlock(&list.lock);
		if (done)
			break;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
			vshot_pwa_error_out(err, err_len,
					    "the PipeWire session did not answer within %d ms",
					    timeout_ms);
			goto out;
		}

		api->thread_loop_lock(list.loop);
		api->thread_loop_timed_wait(list.loop, VSHOT_PWA_NEGOTIATION_SLICE_SEC);
		api->thread_loop_unlock(list.loop);
	}

	/* Resolve the two halves: the first Stream/Output node whose client's pid
	 * is the one asked for.  A client with several output streams (a browser
	 * with several tabs playing) is one application, and the first node
	 * carries its mix. */
	pthread_mutex_lock(&list.lock);
	for (int n = 0; n < list.node_count && result != 1; n++) {
		for (int c = 0; c < list.client_count; c++) {
			if (strcmp(list.nodes[n].client_id, list.clients[c].id) == 0 &&
			    strcmp(list.clients[c].pid, list.pid) == 0) {
				snprintf(out, (size_t)out_len, "%s", list.nodes[n].serial);
				result = 1;
				break;
			}
		}
	}
	if (result != 1)
		result = 0;
	pthread_mutex_unlock(&list.lock);

out:
	if (locked)
		api->thread_loop_unlock(list.loop);
	if (list.loop != NULL && list.started)
		api->thread_loop_stop(list.loop);
	if (core != NULL)
		api->core_disconnect(core);
	if (context != NULL)
		api->context_destroy(context);
	if (list.loop != NULL)
		api->thread_loop_destroy(list.loop);
	pthread_mutex_destroy(&list.lock);
	return result;
}
