#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include "libvx.h"

#ifdef __cplusplus
#pragma error
#endif

#ifdef DEBUG
#define dprintf(...) { printf("%s:%d %-30s ", __FILE__, __LINE__, __func__); printf(__VA_ARGS__); }
#else
#define dprintf(...)
#endif

static bool initialized = false;
static int retry_count = 100;
static vx_log_callback log_cb = NULL;

struct vx_frame
{
	int width;
	int height;
	vx_pix_fmt pix_fmt;

	void* buffer;
};

#define FRAME_QUEUE_SIZE 16
#define FRAME_BUFFER_PADDING 4096
#define LOG_TRACE_BUFSIZE 4096

struct vx_video
{
	AVFormatContext* fmt_ctx;
	AVCodecContext* video_codec_ctx;
	AVCodecContext* audio_codec_ctx;
	SwrContext* swr_ctx;
	enum AVPixelFormat hw_pix_fmt;
	AVBufferRef* hw_device_ctx;

	int video_stream;
	int audio_stream;

	int out_sample_rate;
	int out_channels;
	vx_sample_fmt out_sample_format;

	bool frame_deferred;
	long frame_count;
	vx_audio_callback audio_cb;
	void* audio_user_data;

	int swr_channels;
	int swr_sample_rate;
	int64_t swr_channel_layout;
	int swr_sample_format;

	uint8_t** audio_buffer;
	int audio_line_size;
	int max_samples;
	int samples_since_last_frame;

	int num_queue;
	AVFrame* frame_queue[FRAME_QUEUE_SIZE + 1];

	int open_flags;
	double last_ts;
};

static vx_log_level av_to_vx_log_level(const int level)
{
	// See: lavu_log_constants
	switch (level) {
	case AV_LOG_QUIET:
		return VX_LOG_NONE;

	case AV_LOG_PANIC:
	case AV_LOG_FATAL:
		return VX_LOG_FATAL;

	case AV_LOG_ERROR:
		return VX_LOG_ERROR;

	case AV_LOG_INFO:
	case AV_LOG_VERBOSE:
		return VX_LOG_INFO;

	case AV_LOG_DEBUG:
		return VX_LOG_DEBUG;

	default:
		return VX_LOG_NONE;
	}
}

static void vx_log_set_cb(vx_log_callback cb)
{
	log_cb = cb;
}

static void vx_log_cb(const void* avcl, int level, const char* fmt, void* vl)
{
	// This level of detail won't be needed and results in a huge number of callbacks
	if (!log_cb || level == AV_LOG_TRACE) {
		return;
	}

	if (avcl) {
		// Unused parameter
	}

	const char message[LOG_TRACE_BUFSIZE] = { NULL };

	if (vsprintf_s(&message, LOG_TRACE_BUFSIZE, fmt, vl) > 0) {
		log_cb(message, av_to_vx_log_level(level));
	}
}

static enum AVPixelFormat vx_to_av_pix_fmt(vx_pix_fmt fmt)
{
	enum AVPixelFormat formats[] = { AV_PIX_FMT_RGB24, AV_PIX_FMT_GRAY8, AV_PIX_FMT_BGRA };
	return formats[fmt];
}

static int vx_enqueue_qsort_fn(AVFrame* a, AVFrame* b)
{
	const AVFrame* frame_a = *(AVFrame**)a;
	const AVFrame* frame_b = *(AVFrame**)b;

	return (int)(frame_b->best_effort_timestamp - frame_a->best_effort_timestamp);
}

static void vx_enqueue(vx_video* video, AVFrame* frame)
{
	video->frame_queue[video->num_queue++] = frame;
	qsort(video->frame_queue, video->num_queue, sizeof(AVFrame*), &vx_enqueue_qsort_fn);
}

static AVFrame* vx_dequeue(vx_video* video)
{
	return video->frame_queue[--video->num_queue];
}

static AVFrame* vx_get_first_queue_item(const vx_video* video)
{
	return video->frame_queue[video->num_queue - 1];
}

static bool use_hw(const vx_video video, const AVCodec* codec)
{
	if (video.open_flags & VX_OF_HW_ACCEL_ALL)
		return true;

	if (video.open_flags & VX_OF_HW_ACCEL_720 && vx_get_height(&video) >= 720)
		return true;

	if (video.open_flags & VX_OF_HW_ACCEL_1080 && vx_get_height(&video) >= 1080)
		return true;

	if (video.open_flags & VX_OF_HW_ACCEL_1440 && vx_get_height(&video) >= 1440)
		return true;

	if (video.open_flags & VX_OF_HW_ACCEL_2160 && vx_get_height(&video) >= 2160)
		return true;

	if (video.open_flags & VX_OF_HW_ACCEL_HEVC && codec->id == AV_CODEC_ID_HEVC)
		return true;

	if (video.open_flags & VX_OF_HW_ACCEL_H264 && codec->id == AV_CODEC_ID_H264)
		return true;

	return false;
}

static const AVCodecHWConfig* get_hw_config(const AVCodec* codec)
{
	enum AVHWDeviceType type_priority[] = {
		AV_HWDEVICE_TYPE_VDPAU,
		AV_HWDEVICE_TYPE_D3D11VA,
		AV_HWDEVICE_TYPE_CUDA,
		AV_HWDEVICE_TYPE_VAAPI,
		AV_HWDEVICE_TYPE_DXVA2,
		AV_HWDEVICE_TYPE_QSV,
		AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
		AV_HWDEVICE_TYPE_DRM,
		AV_HWDEVICE_TYPE_OPENCL,
		AV_HWDEVICE_TYPE_MEDIACODEC,
	};

	for (int j = 0; j < sizeof(type_priority) / sizeof(enum AVHWDeviceType); j++)
	{
		enum AVHWDeviceType target_type = type_priority[j];

		for (int i = 0;; i++)
		{
			const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i++);

			if (config != NULL && config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
				config->device_type == target_type)
			{
				dprintf("found hardware config: %s\n", av_hwdevice_get_type_name(config->device_type));
				return config;
			}

			if (config == NULL) break;
		}
	}

	return NULL;
}

static int hw_decoder_init(vx_video* me, AVCodecContext* ctx, const enum AVHWDeviceType type)
{
	int err = 0;

	if ((err = av_hwdevice_ctx_create(&me->hw_device_ctx, type, NULL, NULL, 0)) < 0)
	{
		dprintf("Failed to create specified HW device.\n");
		return err;
	}

	ctx->hw_device_ctx = av_buffer_ref(me->hw_device_ctx);

	// Decoder does not assign sufficient pool size for mpeg2
	if (ctx->codec_id == AV_CODEC_ID_MPEG2VIDEO && ctx->extra_hw_frames < 16) {
		ctx->extra_hw_frames = 16;
	}

	return err;
}

static bool find_stream_and_open_codec(vx_video* me, enum AVMediaType type,
	int* out_stream, AVCodecContext** out_codec_ctx, vx_error* out_error)
{
	AVCodec* codec;

	*out_stream = av_find_best_stream(me->fmt_ctx, type, -1, -1, &codec, 0);

	if (*out_stream < 0)
	{
		if (*out_stream == AVERROR_STREAM_NOT_FOUND)
			*out_error = VX_ERR_VIDEO_STREAM;

		if (*out_stream == AVERROR_DECODER_NOT_FOUND)
			*out_error = VX_ERR_FIND_CODEC;

		return false;
	}

	// Get a pointer to the codec context for the video stream
	*out_codec_ctx = me->fmt_ctx->streams[*out_stream]->codec;

	// Find and enable any hardware acceleration support
	const AVCodecHWConfig* hw_config = use_hw(*me, codec) ? get_hw_config(codec) : NULL;

	if (hw_config != NULL)
	{
		hw_decoder_init(me, *out_codec_ctx, hw_config->device_type);
		me->hw_pix_fmt = hw_config->pix_fmt;
	}

	// Open codec
	if (avcodec_open2(*out_codec_ctx, codec, NULL) < 0)
	{
		*out_error = VX_ERR_OPEN_CODEC;
		return false;
	}

	return true;
}

vx_error vx_open(vx_video** video, const char* filename, int flags)
{
	if (!initialized) {
		// Log messages with this level, or lower, will be send to stderror
		av_log_set_level(AV_LOG_FATAL);
		if (&vx_log_cb) {
			// Redirect all libav error messages to a callback instead of stderror
			av_log_set_callback(&vx_log_cb);
		}
		initialized = true;
	}

	vx_video* me = calloc(1, sizeof(vx_video));

	if (!me)
		return VX_ERR_ALLOCATE;

	me->hw_pix_fmt = AV_PIX_FMT_NONE;
	me->open_flags = flags;
	me->last_ts = -1;

	vx_error error = VX_ERR_UNKNOWN;

	// Open stream
	int open_result = avformat_open_input(&me->fmt_ctx, filename, NULL, NULL);
	if (open_result != 0) {
		if (open_result == AVERROR(ENOENT)) {
			error = VX_ERR_FILE_NOT_FOUND;
		}
		else {
			error = VX_ERR_OPEN_FILE;
		}
		goto cleanup;
	}

	// Get stream information
	if (avformat_find_stream_info(me->fmt_ctx, /*&options*/ NULL) < 0) {
		error = VX_ERR_STREAM_INFO;
		goto cleanup;
	}

	// Find video and audio streams and open respective codecs
	if (!find_stream_and_open_codec(me, AVMEDIA_TYPE_VIDEO, &me->video_stream, &me->video_codec_ctx, &error)) {
		goto cleanup;
	}

	if (!find_stream_and_open_codec(me, AVMEDIA_TYPE_AUDIO, &me->audio_stream, &me->audio_codec_ctx, &error)) {
		dprintf("no audio stream\n");
	}

	*video = me;
	return VX_ERR_SUCCESS;

cleanup:
	vx_close(me);
	return error;
}

void vx_close(vx_video* video)
{
	if (!video) {
		return;
	}

	if (video->swr_ctx)
		swr_free(&video->swr_ctx);

	if (video->fmt_ctx)
		avformat_free_context(video->fmt_ctx);

	for (int i = 0; i < video->num_queue; i++) {
		av_frame_unref(video->frame_queue[i]);
		av_frame_free(&video->frame_queue[i]);
	}

	free(video);
}

static bool vx_read_frame(AVFormatContext* fmt_ctx, AVPacket* packet, int stream)
{
	// try to read a frame, if it can't be read, skip ahead a bit and try again
	int64_t last_fp = avio_tell(fmt_ctx->pb);

	for (int i = 0; i < 1024; i++) {
		int ret = av_read_frame(fmt_ctx, packet);

		// success
		if (ret == 0)
			return true;

		// eof, no need to retry
		if (ret == AVERROR_EOF || avio_feof(fmt_ctx->pb))
			return false;

		// other error, might be a damaged stream, seek forward a couple bytes and try again
		if ((i % 10) == 0) {
			int64_t fp = avio_tell(fmt_ctx->pb);

			if (fp <= last_fp)
				fp = last_fp + 100 * (int64_t)i;

			dprintf("retry: @%" PRId64 "\n", fp);
			avformat_seek_file(fmt_ctx, stream, fp + 100, fp + 512, fp + 1024 * 1024, AVSEEK_FLAG_BYTE | AVSEEK_FLAG_ANY);

			last_fp = fp;
		}
	}

	return false;
}

vx_error vx_count_frames(vx_video* me, int* out_num_frames)
{
	int num_frames = 0;

	AVPacket* packet = av_packet_alloc();
	if (!packet) {
		return VX_ERR_ALLOCATE;
	}

	while (true) {
		if (!vx_read_frame(me->fmt_ctx, packet, me->video_stream)) {
			break;
		}

		if (packet->stream_index == me->video_stream) {
			num_frames++;
		}

		av_packet_unref(packet);
	}

	av_packet_unref(packet);
	av_packet_free(&packet);

	*out_num_frames = num_frames;

	return VX_ERR_SUCCESS;
}

int vx_get_width(const vx_video* me)
{
	return me->video_codec_ctx->width;
}

int vx_get_height(const vx_video* me)
{
	return me->video_codec_ctx->height;
}

long long vx_get_file_position(const vx_video* video)
{
	return video->fmt_ctx->pb->pos;
}

long long vx_get_file_size(const vx_video* video)
{
	return avio_size(video->fmt_ctx->pb);
}

int vx_get_audio_sample_rate(const vx_video* me)
{
	if (!me->audio_codec_ctx)
		return 0;

	return me->audio_codec_ctx->sample_rate;
}

int vx_get_audio_present(const vx_video* me)
{
	return me->audio_codec_ctx ? 1 : 0;
}

int vx_get_audio_channels(const vx_video* me)
{
	if (!me->audio_codec_ctx)
		return 0;

	return me->audio_codec_ctx->channels;
}

static double vx_timestamp_to_seconds_internal(const vx_video* video, const int stream_type, const long long ts)
{
	return (double)ts * av_q2d(video->fmt_ctx->streams[stream_type]->time_base);
}

double vx_timestamp_to_seconds(const vx_video* video, const long long ts)
{
	return vx_timestamp_to_seconds_internal(video, video->video_stream, ts);
}

double vx_estimate_timestamp(vx_video* video, const int stream_type, const int64_t pts)
{
	double ts_estimated = 0.0;
	double ts_seconds = vx_timestamp_to_seconds_internal(video, stream_type, pts);
	double ts_delta = ts_seconds - video->last_ts;

	// Not all codecs supply a timestamp, or they supply values that don't progress nicely
	// So sometimes we need to estimate based on FPS
	if (pts == AV_NOPTS_VALUE || ts_delta <= 0 || ts_delta >= 2) {
		// Initial timestamp should be zero
		if (video->last_ts < 0 || video->frame_count == 0) {
			video->last_ts = 0;
		}
		else if (stream_type == video->video_stream) {
			float fps = 0;
			if (vx_get_frame_rate(video, &fps) == VX_ERR_SUCCESS) {
				double delta = 1.0 / fps;

				ts_estimated += delta;
			}
		}
	}
	else if (pts > 0) {
		// Use the decoded timestamp
		ts_estimated = ts_delta;
	}
	else {
		video->last_ts = ts_seconds;
	}

	return stream_type == video->video_stream
		? video->last_ts += ts_estimated
		: video->last_ts + ts_estimated;
}

static vx_error vx_decode_frame(vx_video* me, static AVFrame* out_frame_buffer[50], int* out_frames_count, int* out_stream_idx)
{
	vx_error ret = VX_ERR_UNKNOWN;
	AVCodecContext* codec_ctx = NULL;
	AVPacket* packet = NULL;
	AVFrame* frame = NULL;
	int frame_count = 0;
	*out_frames_count = 0;
	*out_stream_idx = -1;

	packet = av_packet_alloc();
	if (!packet) {
		ret = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	// Get a packet, which will usually be a single video frame, or several complete audio frames
	vx_read_frame(me->fmt_ctx, packet, me->video_stream);

	// Only attempt to deocde packets from the two streams that have been selected
	if (packet->data && packet->stream_index != me->video_stream && packet->stream_index != me->audio_stream)
	{
		ret = VX_ERR_SUCCESS;
		goto cleanup;
	}

	if (packet->data) {
		*out_stream_idx = packet->stream_index;
		codec_ctx = packet->stream_index == me->video_stream
			? me->video_codec_ctx
			: me->audio_codec_ctx;
	}
	else {
		*out_stream_idx = me->video_stream;
		codec_ctx = me->video_codec_ctx;
	}

	// The decoder may still hold a couple of cached frames, so even if the end of the file has been
	// reached and no packet is returned, it still needs to be sent in order to flush the decoder
	int result = avcodec_send_packet(codec_ctx, packet);
	if (result != 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
		ret = VX_ERR_DECODE_VIDEO;
		goto cleanup;
	}

	// Store all frames returned by the decoder
	while (result >= 0) {
		frame = av_frame_alloc();
		result = avcodec_receive_frame(codec_ctx, frame);

		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
			break;
		}
		else if (result < 0) {
			ret = VX_ERR_DECODE_VIDEO;
			goto cleanup;
		}
		else {
			out_frame_buffer[frame_count++] = frame;
		}
	}

	*out_frames_count = frame_count;
	ret = VX_ERR_SUCCESS;

cleanup:
	if (frame) {
		av_frame_unref(frame);
		av_frame_free(&frame);
	}

	if (packet && packet->data)
		av_packet_unref(packet);
	av_packet_free(&packet);

	return ret;
}

static vx_error vx_scale_frame(const AVFrame* frame, vx_frame* vxframe)
{
	vx_error ret = VX_ERR_UNKNOWN;
	int av_pixfmt = vx_to_av_pix_fmt(vxframe->pix_fmt);

	struct SwsContext* sws_ctx = sws_getContext(
		frame->width, frame->height, frame->format,
		vxframe->width, vxframe->height, av_pixfmt,
		SWS_FAST_BILINEAR, NULL, NULL, NULL);

	if (!sws_ctx) {
		ret = VX_ERR_SCALING;
		goto cleanup;
	}

	assert(frame->data);

	int fmtBytesPerPixel[3] = { 3, 1, 4 };

	uint8_t* pixels[3] = { vxframe->buffer, 0, 0 };
	int pitch[3] = { fmtBytesPerPixel[vxframe->pix_fmt] * vxframe->width, 0, 0 };

	sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, pixels, pitch);

	sws_freeContext(sws_ctx);

	return VX_ERR_SUCCESS;

cleanup:
	return ret;
}

vx_error vx_frame_process_audio(vx_video* me, AVFrame* frame)
{
	double ts = vx_estimate_timestamp(me, me->audio_stream, frame->best_effort_timestamp);
	int dst_sample_count = (int)av_rescale_rnd(frame->nb_samples, me->out_sample_rate, me->audio_codec_ctx->sample_rate, AV_ROUND_UP);

	if (me->swr_channels != me->audio_codec_ctx->channels || me->swr_channel_layout != me->audio_codec_ctx->channel_layout
		|| me->swr_sample_rate != me->audio_codec_ctx->sample_rate || me->swr_sample_format != me->audio_codec_ctx->sample_fmt)
	{
		dprintf("audio format changed\n");
		dprintf("channels:       %d -> %d\n", me->swr_channels, me->audio_codec_ctx->channels);
		dprintf("channel layout: %08"PRIx64" -> %08"PRIx64"\n", me->swr_channel_layout, me->audio_codec_ctx->channel_layout);
		dprintf("sample rate:    %d -> %d\n", me->swr_sample_rate, me->audio_codec_ctx->sample_rate);
		dprintf("sample format:  %d -> %d\n", me->swr_sample_format, me->audio_codec_ctx->sample_fmt);

		// Reinitialize swr_ctx if the audio codec magically changed parameters
		vx_set_audio_params(me, me->out_sample_rate, me->out_channels, me->out_sample_format, me->audio_cb, me->audio_user_data);
	}

	int swrret = swr_convert(me->swr_ctx, me->audio_buffer, dst_sample_count, (const uint8_t**)frame->data, frame->nb_samples);

	if (swrret < 0) {
		return VX_ERR_RESAMPLE_AUDIO;
	}

	me->audio_cb(me->audio_buffer[0], swrret, ts, me->audio_user_data);
	me->samples_since_last_frame += swrret;

	// Defer video frame until later if we've reached max samples (if set)
	// to allow the application to do additional audio processing 
	if (me->max_samples > 0 && me->samples_since_last_frame >= me->max_samples)
	{
		me->samples_since_last_frame = 0;
		me->frame_deferred = true;
		return VX_ERR_FRAME_DEFERRED;
	}

	return VX_ERR_SUCCESS;
}

vx_error vx_queue_frames(vx_video* me)
{
	vx_error ret = VX_ERR_SUCCESS;
	static AVFrame* frame_buffer[50] = { NULL };
	int frame_idx = 0;
	int frame_count = 0;
	me->frame_deferred = false;

	// Allow for a little room in the frame queue in case more than one video frame is decoded
	while (ret == VX_ERR_SUCCESS && me->num_queue < FRAME_QUEUE_SIZE - 1) {
		int stream_idx = -1;

		ret = vx_decode_frame(me, &frame_buffer, &frame_count, &stream_idx);

		// Process any returned frames before returning an error
		if (ret != VX_ERR_SUCCESS && frame_count <= 0)
			goto cleanup;

		// Only a single video frame is usually returned, but there may be several audio frames
		for (int i = 0; i < frame_count; i++) {
			AVFrame* frame = frame_buffer[i];

			if (stream_idx == me->video_stream) {
				// Video frame
				vx_enqueue(me, frame);
			}
			else if (stream_idx == me->audio_stream) {
				// Audio frame (and audio is enabled)
				if (me->audio_cb) {
					vx_error result = vx_frame_process_audio(me, frame);

					// The rest of the buffer must be processed, even if max samples has been reached
					if (result != VX_ERR_SUCCESS) {
						if (result == VX_ERR_FRAME_DEFERRED) {
							ret = result;
						}
						else {
							ret = result;
							frame_idx = i;
							goto cleanup;
						}
					}
				}

				// Audio data has already been transferred and can be freed here
				// Video frames are queued and freed later
				av_frame_unref(frame);
				av_frame_free(&frame);
			}
			else {
				ret = VX_ERR_UNKNOWN;
				frame_idx = i;
				goto cleanup;
			}
		}
	}

	return ret;

cleanup:
	for (int i = frame_idx; i < frame_count; i++) {
		AVFrame* frame = frame_buffer[i];
		if (frame) {
			av_frame_unref(frame);
			av_frame_free(&frame);
		}
	}

	return ret;
}

vx_error vx_frame_step_internal(vx_video* me, double* out_timestamp_seconds, vx_frame_flag* out_flags)
{
	vx_error ret = VX_ERR_UNKNOWN;
	AVFrame* frame = NULL;

	// Free the first item in the queue (if any)
	// Don't dequeue a frame if the last frame was audio
	if (!me->frame_deferred && me->num_queue > 0) {
		frame = vx_dequeue(me);

		if (!frame) {
			return VX_ERR_UNKNOWN;
		}

		av_frame_unref(frame);
		av_frame_free(&frame);
	}

	// (Re)fill the frame queue
	ret = vx_queue_frames(me);

	// Ensure the queue is processed before returning any errors
	if (ret != VX_ERR_FRAME_DEFERRED && me->num_queue > 0) {
		frame = vx_get_first_queue_item(me);

		*out_timestamp_seconds = vx_estimate_timestamp(me, me->video_stream, frame->best_effort_timestamp);
		*out_flags = frame->pict_type == AV_PICTURE_TYPE_I ? VX_FF_KEYFRAME : 0;
		if (frame->pkt_pos != -1)
			*out_flags |= frame->pkt_pos < 0 ? VX_FF_BYTE_POS_GUESSED : 0;
		*out_flags |= frame->pts > 0 ? VX_FF_HAS_PTS : 0;

		ret = VX_ERR_SUCCESS;
	}
	else if (ret == VX_ERR_FRAME_DEFERRED) {
		*out_timestamp_seconds = me->last_ts;
	}

	return ret;
}

vx_error vx_frame_step(vx_video* me, double* out_timestamp_seconds, vx_frame_flag* out_flags)
{
	vx_error first_error = VX_ERR_SUCCESS;

	for (int i = 0; i < retry_count; i++)
	{
		vx_error e = vx_frame_step_internal(me, out_timestamp_seconds, out_flags);

		if (!(e == VX_ERR_UNKNOWN || e == VX_ERR_VIDEO_STREAM || e == VX_ERR_DECODE_VIDEO ||
			e == VX_ERR_DECODE_AUDIO || e == VX_ERR_NO_AUDIO || e == VX_ERR_RESAMPLE_AUDIO))
		{
			me->frame_count++;

			return e;
		}

		first_error = first_error != VX_ERR_SUCCESS ? first_error : e;
	}

	return first_error;
}

vx_error vx_frame_transfer_data(const vx_video* video, vx_frame* frame)
{
	AVFrame* av_frame = NULL;

	if (video->num_queue <= 0)
		return VX_ERR_EOF;

	// Get the first item from the queue, but do not dequeue
	av_frame = vx_get_first_queue_item(video);
	if (!av_frame)
		goto cleanup;

	// Copy the frame from GPU memory if it has been hardware decoded
	bool hw_decoded = av_frame->hw_frames_ctx;
	if (hw_decoded)
	{
		AVFrame* sw_frame = av_frame_alloc();

		if (!sw_frame)
			goto cleanup;

		if (av_hwframe_transfer_data(sw_frame, av_frame, 0) < 0)
		{
			dprintf("Error transferring the data to system memory\n");
			goto cleanup;
		}

		av_frame = sw_frame;
	}

	// Fill the buffer
	vx_scale_frame(av_frame, frame);

	if (hw_decoded) {
		av_frame_unref(av_frame);
		av_frame_free(&av_frame);
	}

	return VX_ERR_SUCCESS;

cleanup:
	if (av_frame) {
		av_frame_unref(av_frame);
		av_frame_free(&av_frame);
	}

	return VX_ERR_UNKNOWN;
}

vx_error vx_get_frame_rate(const vx_video* video, float* out_fps)
{
	AVRational rate = video->fmt_ctx->streams[video->video_stream]->avg_frame_rate;

	if (rate.num == 0 || rate.den == 0)
		return VX_ERR_FRAME_RATE;

	*out_fps = (float)av_q2d(rate);
	return VX_ERR_SUCCESS;
}

vx_error vx_get_duration(const vx_video* video, float* out_duration)
{
	*out_duration = (float)video->fmt_ctx->duration / (float)AV_TIME_BASE;
	return VX_ERR_SUCCESS;
}

bool vx_get_hw_context_present(const vx_video* video)
{
	return video->hw_device_ctx != NULL;
}

vx_error vx_get_pixel_aspect_ratio(const vx_video* video, float* out_par)
{
	AVRational par = video->video_codec_ctx->sample_aspect_ratio;
	if (par.num == 0 && par.den == 1)
		return VX_ERR_PIXEL_ASPECT;

	*out_par = (float)av_q2d(par);
	return VX_ERR_SUCCESS;
}

vx_frame* vx_frame_create(int width, int height, vx_pix_fmt pix_fmt)
{
	vx_frame* me = calloc(1, sizeof(vx_frame));

	if (!me)
		goto error;

	me->width = width;
	me->height = height;
	me->pix_fmt = pix_fmt;

	int av_pixfmt = vx_to_av_pix_fmt(pix_fmt);
	// Includes some padding as a workaround for a bug in swscale (?) where it overreads the buffer
	int size = av_image_get_buffer_size(av_pixfmt, width, height, 1) + FRAME_BUFFER_PADDING;

	if (size <= 0)
		goto error;

	me->buffer = av_mallocz(size);

	if (!me->buffer)
		goto error;

	return me;

error:
	if (me)
		free(me);

	return NULL;
}

void vx_frame_destroy(vx_frame* me)
{
	av_free(me->buffer);
	free(me);
}

void* vx_frame_get_buffer(vx_frame* frame)
{
	return frame->buffer;
}

int vx_frame_get_buffer_size(const vx_frame* frame)
{
	int av_pixfmt = vx_to_av_pix_fmt(frame->pix_fmt);
	return av_image_get_buffer_size(av_pixfmt, frame->width, frame->height, 1) + FRAME_BUFFER_PADDING;
}

vx_error vx_set_audio_max_samples_per_frame(vx_video* me, int max_samples)
{
	me->max_samples = max_samples;
	return VX_ERR_SUCCESS;
}

vx_error vx_set_audio_params(vx_video* me, int sample_rate, int channels, vx_sample_fmt format, vx_audio_callback cb, void* user_data)
{
	vx_error err = VX_ERR_UNKNOWN;

	if (me->audio_stream < 0)
		return VX_ERR_NO_AUDIO;

	me->audio_cb = cb;
	me->out_channels = channels;
	me->out_sample_rate = sample_rate;
	me->out_sample_format = format;
	me->audio_user_data = user_data;

	if (me->swr_ctx)
		swr_free(&me->swr_ctx);

	if (me->audio_buffer) {
		av_freep(&me->audio_buffer[0]);
		av_freep(&me->audio_buffer);
	}

	const AVCodecContext* ctx = me->audio_codec_ctx;

	enum AVSampleFormat avfmt = format == VX_SAMPLE_FMT_FLT ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;

	int64_t src_channel_layout = ctx->channel_layout != 0 ? ctx->channel_layout :
		av_get_default_channel_layout(ctx->channels);

	me->swr_ctx = swr_alloc_set_opts(NULL, av_get_default_channel_layout(channels),
		avfmt, me->out_sample_rate, src_channel_layout, ctx->sample_fmt, ctx->sample_rate, 0, NULL);

	me->swr_channels = ctx->channels;
	me->swr_channel_layout = ctx->channel_layout;
	me->swr_sample_rate = ctx->sample_rate;
	me->swr_sample_format = ctx->sample_fmt;

	if (!me->swr_ctx) {
		err = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	swr_init(me->swr_ctx);

	int ret = av_samples_alloc_array_and_samples(&me->audio_buffer, &me->audio_line_size, channels, sample_rate * 4, avfmt, 0);

	if (ret < 0) {
		err = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	return VX_ERR_SUCCESS;

cleanup:

	if (me->swr_ctx)
		swr_free(&me->swr_ctx);

	if (me->audio_buffer) {
		av_freep(&me->audio_buffer[0]);
		av_freep(&me->audio_buffer);
	}

	return err;
}