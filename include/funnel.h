#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * libfunnel core API
 */

/**
 * @brief A libfunnel context
 *
 * As libfunnel uses no global state, each libfunnel context is completely
 * independent. A context encapsulates a connection to the PipeWire daemon and
 * a background processing thread that handles all PipeWire events and
 * communication.
 */
typedef struct funnel_ctx funnel_ctx;

/**
 * A PipeWire video stream
 *
 * A stream encapsulates a PipeWire node and a single output port. A stream can
 * be configured with a given resolution and choice of formats. When it is
 * connected to an input port, it will deliver frames to it.
 *
 * A libfunnel context can have multiple streams, which all are managed through
 * the shared PipeWire daemon connection and which share the same thread loop.
 */
typedef struct funnel_stream funnel_stream;

/**
 * A video buffer (frame)
 *
 * A buffer encapsulates a video frame of a given format and dimensions. Buffers
 * are dynamically allocated and re-allocated when a stream negotiates its
 * format and properties, as needed by the consumer it is connected to, or when
 * the producer changes the stream confiiguration.
 *
 * Buffers are allocated and owned by a stream and are re-used while video is
 * being delivered. Buffers are either empty (ready for use), dequeued (owned by
 * the API user, and being rendered or written to), or enqueued (ready for use
 * and delivery to the consumer). When the consumer is done with a buffer, it is
 * returned to libfunnel and becomes empty again.
 *
 * Since the stream can re-negotiate at any time (through the background
 * thread), a dequeued buffer might become invalid as new buffers were
 * allocated. If this happens, the dequeued buffer is not freed immediately, so
 * it can still be written or rendered to as normal. When such a stale buffer is
 * enqueued, it will be discarded and freed instead.
 */
typedef struct funnel_buffer funnel_buffer;

/**
 * A rational frame rate
 */
struct funnel_fraction {
    uint32_t num;
    uint32_t den;
};

/**
 * Indicates that the frame rate is variable
 */
static const struct funnel_fraction FUNNEL_RATE_VARIABLE = {0, 1};

/**
 * Helper to create a funnel_fraction
 *
 * @param num Numerator
 * @param den Denominator
 * @returns The funnel_fraction
 */
static inline struct funnel_fraction FUNNEL_FRACTION(uint32_t num,
                                                     uint32_t den) {
    return (struct funnel_fraction){num, den};
}

/**
 * A user callback for buffer creation/destruction
 *
 * @param opaque Opaque user data pointer
 * @param stream Stream for this buffer @borrowed
 * @param buf Buffer being allocated or freed @borrowed
 */
typedef void (*funnel_buffer_callback)(void *opaque,
                                       struct funnel_stream *stream,
                                       struct funnel_buffer *buf);

/**
 * Synchronization modes for the frame pacing
 */
enum funnel_mode {
    /**
     * Produce frames asynchronously to the consumer.
     *
     * In this mode, libfunnel calls never block and you
     * must be able to handle the lack of a buffer (by
     * skipping rendering/copying to it). This mode only
     * makes sense if your application is FPS-limited by
     * some other consumer (for example, if it renders to
     * the screen, usually with VSync). You should configure
     * the frame rate you expect to produce frames at with
     * `funnel_stream_set_rate()`.
     *
     * This mode essentially behaves like triple buffering.
     * Whenever the PipeWire cycle runs, the consumer will
     * receive the frame that was most recently submitted
     * to funnel_stream_enqueue().
     */
    FUNNEL_ASYNC,
    /**
     * Produce frames synchronously to the consumer with
     * double buffering.
     *
     * In this mode, after a frame is produced, it is
     * queued to be sent out to the consumer in the next
     * PipeWire process cycle, and you may immediately
     * dequeue a new buffer to start rendering the next
     * frame. libfunnel will block at `funnel_stream_enqueue()`
     * until the previously queued frame has been consumed.
     * In this mode, `funnel_stream_dequeue()` will only
     * block if there are no free buffers (if the consumer is
     * not freeing buffers quickly enough).
     *
     * This mode effectively adds two frames of latency,
     * as up to two frames can be rendered ahead of the
     * PipeWire cycle (one ready to be submitted, and
     * one blocked at `funnel_stream_enqueue()`).
     */
    FUNNEL_DOUBLE_BUFFERED,
    /**
     * Produce frames synchronously to the consumer with
     * single buffering.
     *
     * In this mode, after a frame is produced, it is
     * queued to be sent out to the consumer in the next
     * PipeWire process cycle. When you are ready to begin
     * rendering a new frame, libfunnel will block
     * at `funnel_stream_dequeue()` until the previous frame
     * has been sent to the consumer. In this mode,
     * `funnel_stream_enqueue()` will never block.
     *
     * This mode effectively adds one frame of latency,
     * as only one frame can be rendered ahead of the
     * PipeWire cycle.
     */
    FUNNEL_SINGLE_BUFFERED,
    /**
     * Produce frames synchronously with the PipeWire process
     * cycle.
     *
     * In this mode, `funnel_stream_dequeue()` will wait for
     * the beginning of a PipeWire process cycle, and the
     * process cycle will be blocked until the frame is
     * submitted with `funnel_stream_enqueue()`.
     *
     * This mode provides the lowest possible latency, but
     * is only suitable for applications that do not do much
     * work to render frames (for example, just a copy), as
     * the PipeWire graph will be blocked while the buffer
     * is dequeued. It adds no latency.
     */
    FUNNEL_SYNCHRONOUS,
};

/**
 * Buffer synchronization modes for frames
 *
 * See \ref buffersync for more information on sync modes.
 */
enum funnel_sync {
    /**
     * Use implicit sync only.
     */
    FUNNEL_SYNC_IMPLICIT,

    /**
     * Use explicit sync only.
     */
    FUNNEL_SYNC_EXPLICIT,

    /**
     * Support both implicit and explicit sync.
     */
    FUNNEL_SYNC_BOTH,
};

/**
 * Represents the state of a stream.
 *
 * This maps to a PipeWire pw_stream_state.
 */
enum funnel_stream_state {
    /** The stream is not connected to a consumer */
    FUNNEL_STREAM_STATE_UNCONNECTED = 0,
    /** The stream is currently connecting to a consumer */
    FUNNEL_STREAM_STATE_CONNECTING = 1,
    /** The stream is paused or reconfiguring */
    FUNNEL_STREAM_STATE_PAUSED = 2,
    /** The stream is actively streaming  */
    FUNNEL_STREAM_STATE_STREAMING = 3
};

/**
 * Initialize a Funnel context.
 *
 * This is equivalent to funnel_create() followed by funnel_connect().
 *
 * @param[out] pctx New context @owned
 * @return_err
 * @retval -ECONNREFUSED Failed to connect to PipeWire daemon
 * @retval -EIO Fatal error connecting to PipeWire daemon
 * @retval -EOPNOTSUPP PipeWire daemon version is too old
 */
int funnel_init(struct funnel_ctx **pctx);

/**
 * Create a Funnel context.
 *
 * As multiple Funnel contexts are completely independent, this function has no
 * synchronization requirements.
 *
 * @param[out] pctx New context @owned
 * @return_err
 */
int funnel_new(struct funnel_ctx **pctx);

/**
 * Set the user-friendly application name.
 *
 * This should be the name of your application.
 * It defaults to the process name.
 *
 * This cannot be changed after the context is connected.
 *
 * @sync-int
 *
 * @param ctx Context @borrowed
 * @param app_name Application name @borrowed
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * The context is already connected to PipeWire
 */
int funnel_set_app_name(struct funnel_ctx *ctx, const char *app_name);

/**
 * Set the application ID.
 *
 * This should be the a unique ID for your application, such as
 * a Flatpak reverse-DNS style ID.
 *
 * This cannot be changed after the context is connected.
 *
 * @sync-int
 *
 * @param ctx Context @borrowed
 * @param app_id Application ID @borrowed
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * The context is already connected to PipeWire
 */
int funnel_set_app_id(struct funnel_ctx *ctx, const char *app_id);

/**
 * Set the application version.
 *
 * A version number, such as "1.2.3".
 *
 * This cannot be changed after the context is connected.
 *
 * @sync-int
 *
 * @param ctx Context @borrowed
 * @param app_version Application version @borrowed
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * The context is already connected to PipeWire
 */
int funnel_set_app_version(struct funnel_ctx *ctx, const char *app_version);

/**
 * Connect to PipeWire.
 *
 * @sync-ext
 *
 * @param ctx Context @borrowed
 * @return_err
 * @retval -EINVAL The context is already connected to PipeWire
 * @retval -ECONNREFUSED Failed to connect to PipeWire daemon
 * @retval -EIO Fatal error connecting to PipeWire daemon
 * @retval -EOPNOTSUPP PipeWire daemon version is too old
 */
int funnel_connect(struct funnel_ctx *ctx);

/**
 * Shut down a Funnel context.
 *
 * @sync-ext
 *
 * @param ctx Context @owned
 */
void funnel_shutdown(struct funnel_ctx *ctx);

/**
 * Create a new stream.
 *
 * @sync-int
 *
 * @param ctx Context @borrowed
 * @param name Name of the new stream @borrowed
 * @param[out] pstream New stream @owned-from{ctx}
 * @return_err
 * @retval -EINVAL The context has not been connected to PipeWire yet
 * @retval -EIO The PipeWire context is invalid (fatal error)
 */
int funnel_stream_create(struct funnel_ctx *ctx, const char *name,
                         struct funnel_stream **pstream);

/**
 * Specify callbacks for buffer creation/destruction.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param alloc Callback when a buffer is allocated @borrowed-by{stream}
 * @param free Callback when a buffer is freed @borrowed-by{stream}
 * @param opaque Opaque user pointer
 */
void funnel_stream_set_buffer_callbacks(struct funnel_stream *stream,
                                        funnel_buffer_callback alloc,
                                        funnel_buffer_callback free,
                                        void *opaque);

/**
 * Set the instance for this stream.
 *
 * This should be an identifier to tell apart multiple instances of your
 * application, if it supports multiple concurrent instances (or multiple
 * "documents"). It should be persistent. For example, you can use the
 * "project"/"file" name the user is working with.
 *
 * Do not use a volatile ID such as the process ID, as that will break
 * auto-connection on restart.
 *
 * If you do not have anything that could be used for this purpose, then
 * you must either allow it to be user-configurable, or leave the instance
 * name unset and instead make sure that the stream name is user-configurable,
 * so that users can ensure that streams across multiple instances of your
 * application can always be uniquely identified.
 *
 * This cannot be changed after the stream is first configured.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param instance Instance identifier @borrowed
 * @param user_friendly Whether the instance name is "user friendly"
 *                      and should be included in the default description.
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * Stream is in an invalid state (already configured)
 */
int funnel_stream_set_instance(struct funnel_stream *stream,
                               const char *instance, bool user_friendly);

/**
 * Set the unique ID for this stream.
 *
 * This should be a persistent, unique ID within your instance. It defaults
 * to the stream name, with special characters removed. For example, this
 * could be a UUID if you have such an internal unique identifier for the
 * stream.
 *
 * This cannot be changed after the stream is first configured.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param stream_id Stream ID @borrowed
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * Stream is in an invalid state (already configured)
 */
int funnel_stream_set_unique_id(struct funnel_stream *stream,
                                const char *stream_id);

/**
 * Set the description for this stream.
 *
 * This should be a user-friendly description.
 *
 * Defaults to an automatically generated string using the application
 * name, instance name (if friendly), and stream name.
 *
 * This cannot be changed after the stream is first configured.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param description Stream description @borrowed
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * Stream is in an invalid state (already configured)
 */
int funnel_stream_set_description(struct funnel_stream *stream,
                                  const char *description);

/**
 * Set the media name for this stream.
 *
 * Unlike the other name properties, this *can* be changed
 * while the stream is running. For example, when a stream
 * is playing back a particular video content, the media name
 * can reflect that to the user (e.g. the name of a movie).
 *
 * Call funnel_stream_configure() after this to update the stream.
 *
 * Defaults to the stream name.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param media_name Media name @borrowed
 * @return_err
 * @retval -EINVAL Invalid argument
 */
int funnel_stream_set_media_name(struct funnel_stream *stream,
                                 const char *media_name);

/**
 * Set the frame dimensions for a stream.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param width Width in pixels
 * @param height Height in pixels
 * @return_err
 * @retval -EINVAL Invalid argument
 */
int funnel_stream_set_size(struct funnel_stream *stream, uint32_t width,
                           uint32_t height);

/**
 * Configure the queueing mode for the stream.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param mode Queueing mode for the stream
 * @return_err
 * @retval -EINVAL Invalid argument
 */
int funnel_stream_set_mode(struct funnel_stream *stream, enum funnel_mode mode);

/**
 * Configure the synchronization modes for the stream.
 *
 * See \ref buffersync for more information on sync modes.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param frontend Synchronization mode for the libfunnel API
 * @param backend Synchronization mode for the PipeWire stream
 * @return_err
 * @retval -EINVAL The selected sync combination is invalid for this API
 * @retval -EOPNOTSUPP The API/driver does not support this sync mode
 */
int funnel_stream_set_sync(struct funnel_stream *stream,
                           enum funnel_sync frontend, enum funnel_sync backend);

/**
 * Set the frame rate of a stream.
 *
 * Note: If the default rate is FUNNEL_RATE_VARIABLE, then the minimum rate
 * also needs to be FUNNEL_RATE_VARIABLE. Otherwise, this will cause issues
 * with some PipeWire versions.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param def Default frame rate (FUNNEL_RATE_VARIABLE for no default or
 * variable)
 * @param min Minimum frame rate (FUNNEL_RATE_VARIABLE if variable)
 * @param max Maximum frame rate (FUNNEL_RATE_VARIABLE if variable)
 * @return_err
 * @retval -EINVAL Invalid argument
 */
int funnel_stream_set_rate(struct funnel_stream *stream,
                           struct funnel_fraction def,
                           struct funnel_fraction min,
                           struct funnel_fraction max);

/**
 * Get the currently negotiated frame rate of a stream.
 *
 * @sync-int
 *
 * @param stream Stream @borrowed
 * @param[out] prate Output frame rate
 * @return_err
 * @retval -EINPROGRESS The stream is not yet initialized
 */
int funnel_stream_get_rate(struct funnel_stream *stream,
                           struct funnel_fraction *prate);

/**
 * Clear the supported format list. Used for reconfiguration.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 */
void funnel_stream_clear_formats(struct funnel_stream *stream);

/**
 * Apply the stream configuration and register the stream with PipeWire.
 *
 * If called on an already configured stream, this will update the
 * configuration.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @return_err
 * @retval -EINVAL The stream is in an invalid state (missing settings)
 * @retval -EIO The PipeWire context is invalid or stream creation failed
 */
int funnel_stream_configure(struct funnel_stream *stream);

/**
 * Start running a stream.
 *
 * @sync-int
 *
 * @param stream Stream @borrowed
 * @return_err
 * @retval -EINVAL The stream is in an invalid state (not configured)
 * @retval -EIO The PipeWire context is invalid or stream creation failed
 */
int funnel_stream_start(struct funnel_stream *stream);

/**
 * Stop running a stream.
 *
 * If another thread is blocked on funnel_stream_dequeue(), this will
 * unblock it.
 *
 * @sync-int
 *
 * @param stream Stream @borrowed
 * @return_err
 * @retval -EINVAL The stream is not started
 * @retval -EIO The PipeWire context is invalid
 */
int funnel_stream_stop(struct funnel_stream *stream);

/**
 * Get the current state of a stream.
 *
 * This is primarily intended for informational purposes (for user feedback). It
 * could also be used to change the rendering flow depending on whether the
 * stream is expected to consume frames or not. The return value should be
 * treated as a hint.
 *
 * NOTE: This function should not be used to make any decisions about blocking
 * calls! Since the stream state may change at any time, a running stream could
 * transition to the paused state immediately after this call returns.
 * Similarly, calling funnel_stream_dequeue() immediately after this call could
 * return a buffer even if the stream is paused, or might not return a buffer
 * despite the stream being running.
 *
 * @sync-int
 *
 * @param stream Stream @borrowed
 * @param[out] pstate The returned stream state
 * @return_err
 * @retval -EIO
 *  * The PipeWire context is invalid
 *  * Unknown stream error
 */
int funnel_stream_get_state(struct funnel_stream *stream,
                            enum funnel_stream_state *pstate);

/**
 * Destroy a stream.
 *
 * The stream will be stopped if it is running.
 *
 * @sync-ext
 *
 * @param stream Stream @owned
 */
void funnel_stream_destroy(struct funnel_stream *stream);

/**
 * Dequeue a buffer from a stream.
 *
 * Note that, currently, you may only have one buffer
 * dequeued at a time.
 *
 * @sync-int
 *
 * @param stream Stream @borrowed
 * @param[out] pbuf Buffer that was dequeued @owned-from{stream}
 * @return Whether a buffer was dequeued successfully, or a negative error
 * number on error.
 * @retval 0 No buffer is available
 * @retval 1 A buffer was successfully dequeued
 * @retval -EINVAL Stream is in an invalid state
 * @retval -EBUSY Attempted to dequeue more than one buffer at once
 * @retval -EIO The PipeWire context is invalid
 * @retval -ESHUTDOWN Stream is not started
 */
int funnel_stream_dequeue(struct funnel_stream *stream,
                          struct funnel_buffer **pbuf);

/**
 * Enqueue a buffer to a stream.
 *
 * After this call, the buffer is no longer owned by the user and may not be
 * queued again until it is dequeued.
 *
 * @sync-int
 *
 * @param stream Stream @borrowed
 * @param buf Buffer to enqueue @owned
 * @return Whether a buffer was enqueued successfully, or a negative error
 * number on error.
 * @retval 0 The buffer was dropped because the stream configuration or state
 * changed.
 * @retval 1 The buffer was successfully enqueued.
 * @retval -EINVAL
 *  * Invalid argument
 *  * Stream is in an invalid state (not yet configured)
 *  * Buffer requires sync, but sync was not handled properly
 */
int funnel_stream_enqueue(struct funnel_stream *stream,
                          struct funnel_buffer *buf);

/**
 * Return a buffer to the pool without enqueueing it.
 *
 * After this call, the buffer is no longer owned by the user and may not be
 * queued again until it is dequeued. This will effectively drop one frame in
 * the stream (a PipeWire process cycle will complete with no frame transfer).
 *
 * @sync-int
 *
 * @param stream Stream @borrowed
 * @param buf Buffer to return @owned
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * Stream is in an invalid state (not yet configured)
 */
int funnel_stream_return(struct funnel_stream *stream,
                         struct funnel_buffer *buf);

/**
 * Break out of a frame processing cycle for a stream
 *
 * This call unblocks stream buffer enqueue/dequeue functions. This is useful to
 * break a thread out of blocking calls. The precise behavior depends on the
 * mode:
 *
 * * #FUNNEL_ASYNC: For each call, funnel_stream_dequeue() will return once
 *   without a buffer even if one was available.
 * * #FUNNEL_DOUBLE_BUFFERED: For each call, if funnel_stream_enqueue() would
 *   block, it will instead drop the passed buffer and return immediately.
 * * #FUNNEL_SINGLE_BUFFERED and #FUNNEL_SYNCHRONOUS: For each call,
 *   funnel_stream_dequeue() will return once without a buffer.
 *
 * Note that this function does not force an empty PipeWire process cycle,
 * that is, it won't "skip a frame" in the stream itself. Think of it as
 * skipping a frame loop from the point of view of your code. In other words,
 * it does not forcefully generate any delay.
 *
 * @sync-int
 *
 * @param stream Stream @borrowed
 * @return_err
 * @retval -EINVAL Stream is in an invalid state (not yet configured)
 */
int funnel_stream_skip_frame(struct funnel_stream *stream);

/**
 * Get the dimensions of a Funnel buffer.
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @param[out] pwidth Output width
 * @param[out] pheight Output height
 */
void funnel_buffer_get_size(struct funnel_buffer *buf, uint32_t *pwidth,
                            uint32_t *pheight);

/**
 * Prime a DMA-BUF backed buffer for external GPU import.
 *
 * Some drivers only make a freshly-written imported DMA-BUF visible to a
 * separate GL/EGL consumer after the fd has gone through a CPU sync/mmap path
 * at least once. This is intended for producer-side first use after rendering
 * into the buffer and before enqueueing it to PipeWire.
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @return_err
 * @retval -EINVAL Buffer is invalid or not DMA-BUF backed
 * @retval -errno DMA-BUF sync or mmap failed
 */
int funnel_buffer_prime_dmabuf(struct funnel_buffer *buf);

/**
 * Set an arbitrary user data pointer for a buffer.
 *
 * The user is responsible for managing the lifetime of this object.
 * Generally, you should use funnel_stream_set_buffer_callbacks()
 * to provide buffer creation/destruction callbacks, and set and
 * release the user data pointer in the alloc and free callback
 * respectively.
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @param opaque Opaque user data pointer
 */
void funnel_buffer_set_user_data(struct funnel_buffer *buf, void *opaque);

/**
 * Get an arbitrary user data pointer for a buffer.
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @return The user data pointer
 */
void *funnel_buffer_get_user_data(struct funnel_buffer *buf);

/**
 * Check whether a buffer requires explicit synchronization.
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @retval true if the buffer requires explicit synchronization
 */
bool funnel_buffer_has_sync(struct funnel_buffer *buf);

/**
 * Return whether a buffer is considered efficient for rendering.
 *
 * Buffers are considered efficient when they are not using linear tiling
 * and non-linear tiling is supported by the GPU driver.
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @retval true if the buffer is likely to be efficient to render into
 * @retval false if the buffer is unlikely to be efficient to render into
 */
bool funnel_buffer_is_efficient_for_rendering(struct funnel_buffer *buf);

#ifdef __cplusplus
}
#endif
