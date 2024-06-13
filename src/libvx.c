#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "libvx.h"
#include "util.h"
#include "filter.h"
#include "filtergraph.h"

#ifdef __cplusplus
#pragma error
#endif

#define FRAME_BUFFER_PADDING 4096
#define LOG_TRACE_BUFSIZE 4096
#define TICKS_PER_SECOND (AV_TIME_BASE * 10L)

static bool initialized = false;
static vx_log_callback log_cb = NULL;

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

		case AV_LOG_WARNING:
			return VX_LOG_WARNING;

		case AV_LOG_INFO:
		case AV_LOG_VERBOSE:
			return VX_LOG_INFO;

		case AV_LOG_DEBUG:
			return VX_LOG_DEBUG;

		default:
			return VX_LOG_NONE;
	}
}

void vx_log_set_cb(vx_log_callback cb)
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

	if (vsnprintf(&message, sizeof(message), fmt, vl) > 0) {
		log_cb(message, av_to_vx_log_level(level));
	}
}

static double vx_timestamp_to_seconds(const vx_video* video, const int stream_type, const long long ts)
{
	// Ensure the timestamp won't overflow
	int64_t rescaled_timestamp = av_rescale_rnd(ts, TICKS_PER_SECOND, 1, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX) > INT64_MIN
		? ts
		: 0;

	return (double)rescaled_timestamp * av_q2d(video->fmt_ctx->streams[stream_type]->time_base);
}

static int vx_enqueue_qsort(const void* a, const void* b)
{
	const AVFrame* frame_a = *(const AVFrame**)a;
	const AVFrame* frame_b = *(const AVFrame**)b;

	return (int)(frame_b->best_effort_timestamp - frame_a->best_effort_timestamp);
}

static void vx_enqueue(vx_video* video, AVFrame* frame)
{
	video->frame_queue[video->frame_queue_count++] = frame;
	qsort(video->frame_queue, video->frame_queue_count, sizeof(AVFrame*), &vx_enqueue_qsort);
}

static AVFrame* vx_dequeue(vx_video* video)
{
	return video->frame_queue[--video->frame_queue_count];
}

static AVFrame* vx_get_first_queue_item(const vx_video* video)
{
	return video->frame_queue[video->frame_queue_count - 1];
}

static bool use_hw(const vx_video* video, const AVCodec* codec)
{
	if (video->options.hw_criteria & VX_HW_ACCEL_ALL)
		return true;

	int height = vx_get_height(video);

	if (video->options.hw_criteria & VX_HW_ACCEL_720 && height >= 720)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_1080 && height >= 1080)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_1440 && height >= 1440)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_2160 && height >= 2160)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_HEVC && codec->id == AV_CODEC_ID_HEVC)
		return true;

	if (video->options.hw_criteria & VX_HW_ACCEL_H264 && codec->id == AV_CODEC_ID_H264)
		return true;

	return false;
}

static const AVCodecHWConfig* get_hw_config(const AVCodec* codec)
{
	enum AVHWDeviceType type_priority[] = {
		AV_HWDEVICE_TYPE_VDPAU,
		AV_HWDEVICE_TYPE_D3D11VA,
		AV_HWDEVICE_TYPE_VAAPI,
		AV_HWDEVICE_TYPE_DXVA2,
		AV_HWDEVICE_TYPE_QSV,
		AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
		AV_HWDEVICE_TYPE_DRM,
		AV_HWDEVICE_TYPE_OPENCL,
		AV_HWDEVICE_TYPE_MEDIACODEC,
		AV_HWDEVICE_TYPE_CUDA
	};

	for (int j = 0; j < sizeof(type_priority) / sizeof(enum AVHWDeviceType); j++)
	{
		enum AVHWDeviceType target_type = type_priority[j];

		for (int i = 0;; i++)
		{
			const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);

			if (config != NULL && config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == target_type)
			{
				av_log(NULL, AV_LOG_INFO, "Found hardware config: %s\n", av_hwdevice_get_type_name(config->device_type));
				return config;
			}

			if (config == NULL)
				break;
		}
	}

	return NULL;
}

static int hw_decoder_init(vx_video* me, AVCodecContext* ctx, const enum AVHWDeviceType type)
{
	int err = 0;
	bool codecRequiresExtraFrames = false;
	enum AVCodecID extraHardwareFrameCodecs[] = 
	{ 
		AV_CODEC_ID_HEVC,
		AV_CODEC_ID_MPEG2VIDEO, 
		AV_CODEC_ID_WMV3, 
		AV_CODEC_ID_VC1,
		AV_CODEC_ID_VP9,
	};

	if ((err = av_hwdevice_ctx_create(&me->hw_device_ctx, type, NULL, NULL, 0)) < 0)
	{
		av_log(NULL, AV_LOG_INFO, "Failed to create %s HW device\n", av_hwdevice_get_type_name(type));
		return err;
	}

	ctx->hw_device_ctx = av_buffer_ref(me->hw_device_ctx);

	for (int i = 0; i < sizeof(extraHardwareFrameCodecs) / sizeof(enum AVCodecID); i++)
	{
		codecRequiresExtraFrames = ctx->codec_id == extraHardwareFrameCodecs[i];
		if (codecRequiresExtraFrames)
		{
			break;
		}
	}

	// Decoder does not assign sufficient pool size for codec
	if (codecRequiresExtraFrames && ctx->extra_hw_frames < 16) {
		ctx->extra_hw_frames = 16;
	}

	return err;
}

static bool find_stream_and_open_codec(
	vx_video* me,
	enum AVMediaType type,
	int* out_stream,
	AVCodecContext** out_codec_ctx,
	vx_error* out_error)
{
	AVCodec* codec;
	AVCodecContext* codec_ctx;
	int open_result = 0;

	*out_stream = av_find_best_stream(me->fmt_ctx, type, -1, -1, &codec, 0);

	if (*out_stream < 0)
	{
		if (*out_stream == AVERROR_STREAM_NOT_FOUND)
			*out_error = type == AVMEDIA_TYPE_AUDIO ? VX_ERR_NO_AUDIO : VX_ERR_VIDEO_STREAM;

		if (*out_stream == AVERROR_DECODER_NOT_FOUND)
			*out_error = VX_ERR_FIND_CODEC;

		return false;
	}

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		*out_error = VX_ERR_ALLOCATE;
		return false;
	}

	avcodec_parameters_to_context(codec_ctx, me->fmt_ctx->streams[*out_stream]->codecpar);
	*out_codec_ctx = codec_ctx;

	// Find and enable any hardware acceleration support
	const AVCodecHWConfig* hw_config = use_hw(me, codec) ? get_hw_config(codec) : NULL;

	if (hw_config != NULL)
	{
		hw_decoder_init(me, *out_codec_ctx, hw_config->device_type);
		me->hw_pix_fmt = hw_config->pix_fmt;
	}

	// Open codec
	if ((open_result = avcodec_open2(*out_codec_ctx, codec, NULL)) < 0)
	{
		avcodec_free_context(out_codec_ctx);

		av_log_error_message(open_result, AV_LOG_ERROR, "Unable to open codec: %s\n");

		*out_error = VX_ERR_OPEN_CODEC;
		return false;
	}

	// Set the time base for current stream so it can be read elsewhere
	(*out_codec_ctx)->time_base = me->fmt_ctx->streams[*out_stream]->time_base;

	return true;
}

vx_error vx_init_audio_resampler(vx_video* me, const struct av_audio_params in_params, const vx_audio_params out_params)
{
	vx_error err = VX_ERR_UNKNOWN;
	AVChannelLayout out_layout = { 0 };
	av_channel_layout_default(&out_layout, out_params.channels);

	if (me->swr_ctx)
		swr_free(&me->swr_ctx);

	swr_alloc_set_opts2(
		&me->swr_ctx,
		&out_layout,
		vx_to_av_sample_fmt(out_params.sample_format),
		out_params.sample_rate,
		&in_params.channel_layout,
		in_params.sample_format,
		in_params.sample_rate,
		0,
		NULL);

	if (!me->swr_ctx) {
		err = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	if (swr_init(me->swr_ctx) != 0)
		goto cleanup;

	me->inital_audio_params = in_params;

	return VX_ERR_SUCCESS;

cleanup:
	if (me->swr_ctx)
		swr_free(&me->swr_ctx);

	return err;
}


/// <summary>
/// Retrieve basic video properties.
/// This will advance the demuxer to the end of the file, so it will need
/// resetting if the video is to be read again.
/// </summary>
static vx_error vx_get_properties(const vx_video* video, struct vx_video_info* out_video_info)
{
	const int EXCLUDED_PACKET_FLAGS = AV_PKT_FLAG_CORRUPT | AV_PKT_FLAG_DISCARD | AV_PKT_FLAG_DISPOSABLE;
	int64_t first_timestamp = AV_NOPTS_VALUE;
	int64_t last_timestamp = AV_NOPTS_VALUE;
	int frame_count = 0;

	AVPacket* packet = av_packet_alloc();
	if (!packet) {
		return VX_ERR_ALLOCATE;
	}

	// Iterate through all the data packets (frames) in the video to
	// get a good idea of what can actually be read from the file
	while (av_read_frame(video->fmt_ctx, packet) == 0) {
		// Ignore packets that are not from the selected stream
		// or are marked as corrupt or discarded
		if (packet->stream_index == video->video_stream && !(packet->flags & EXCLUDED_PACKET_FLAGS)) {
			// Ensure the timestamp won't overflow
			int64_t rescaled_timestamp = av_rescale_rnd(packet->dts, TICKS_PER_SECOND, 1, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

			// Use decoded timestamp since presentation timestamp is not
			// always available, and may be out of order
			if (rescaled_timestamp > last_timestamp)
				last_timestamp = packet->dts;

			if (first_timestamp == AV_NOPTS_VALUE)
				first_timestamp = last_timestamp;

			frame_count++;
		}

		av_packet_unref(packet);
	}

	av_packet_unref(packet);
	av_packet_free(&packet);

	// Do the best we can to estimate the frame rate and duration, if required
	const AVStream* video_stream = video->fmt_ctx->streams[video->video_stream];
	AVRational frame_rate = video_stream->avg_frame_rate;
	int64_t duration = llabs(last_timestamp - first_timestamp) > 0 || video_stream->duration == AV_NOPTS_VALUE
		? last_timestamp - first_timestamp
		: video_stream->duration;

	if (frame_rate.num == 0 || frame_rate.den == 0) {
		av_reduce(
			&frame_rate.num,
			&frame_rate.den,
			frame_count * (int64_t)video_stream->time_base.den,
			duration * (int64_t)video_stream->time_base.num,
			60000);
	}

	double duration_seconds = duration > 0
		? (double)duration * av_q2d(video_stream->time_base)
		: frame_count / av_q2d(frame_rate);

	out_video_info->width = vx_get_width(video);
	out_video_info->height = vx_get_height(video);
	out_video_info->adjusted_width = vx_get_adjusted_width(video);
	out_video_info->adjusted_height = vx_get_adjusted_height(video);
	out_video_info->frame_count = frame_count;
	out_video_info->frame_rate = (float)av_q2d(frame_rate);
	out_video_info->duration = (float)duration_seconds;
	out_video_info->has_audio = video->audio_codec_ctx != NULL;
	out_video_info->audio_sample_rate = video->audio_codec_ctx ? video->audio_codec_ctx->sample_rate : 0;
	out_video_info->audio_channels = video->audio_codec_ctx ? video->audio_codec_ctx->ch_layout.nb_channels : 0;

	return VX_ERR_SUCCESS;
}

static vx_error vx_open_internal(vx_video** video, const char* filename, const vx_video_options options)
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
	me->ts_last = 0;
	me->ts_offset = AV_NOPTS_VALUE;
	me->options = options;

	vx_error error = VX_ERR_UNKNOWN;

	// Open stream
	int open_result = avformat_open_input(&me->fmt_ctx, filename, NULL, NULL);
	if (open_result != 0) {
		if (open_result == AVERROR(ENOENT)) {
			error = VX_ERR_FILE_NOT_FOUND;
		}
		else {
			av_log_error_message(open_result, AV_LOG_ERROR, "Unable to open file: %s\n");

			error = VX_ERR_OPEN_FILE;
		}
		goto cleanup;
	}

	// Get stream information
	if (avformat_find_stream_info(me->fmt_ctx, NULL) < 0) {
		error = VX_ERR_STREAM_INFO;
		goto cleanup;
	}

	// Find video and audio streams and open respective codecs
	if (!find_stream_and_open_codec(me, AVMEDIA_TYPE_VIDEO, &me->video_stream, &me->video_codec_ctx, &error)) {
		goto cleanup;
	}

	if (!find_stream_and_open_codec(me, AVMEDIA_TYPE_AUDIO, &me->audio_stream, &me->audio_codec_ctx, &error)) {
		if (me->video_codec_ctx->pix_fmt == AV_PIX_FMT_NONE) {
			// The file doesn't contain video or audio information, but don't exit
			// as we can still get some useful information out by opening the file
			av_log(NULL, AV_LOG_ERROR, "The file does not contain any frame images or audio data\n");
		}
		else {
			av_log(NULL, AV_LOG_INFO, "The file does not contain any audio data\n");
		}
	}

	if (me->video_codec_ctx->pix_fmt != AV_PIX_FMT_NONE) {
		if ((error = vx_initialize_filtergraph(me, AVMEDIA_TYPE_VIDEO, NULL)) != VX_ERR_SUCCESS)
			goto cleanup;
	}

	if (me->audio_codec_ctx && me->options.audio_params.channels > 0) {
		struct av_audio_params params = av_audio_params_from_codec(me->audio_codec_ctx);

		if ((error = vx_init_audio_resampler(me, params, me->options.audio_params)) != VX_ERR_SUCCESS) {
			goto cleanup;
		}

		if ((error = vx_initialize_filtergraph(me, AVMEDIA_TYPE_AUDIO, &params)) != VX_ERR_SUCCESS)
			goto cleanup;
	}

	*video = me;
	return VX_ERR_SUCCESS;

cleanup:
	vx_close(me);
	return error;
}

vx_error vx_open(const char* filename, const vx_video_options options, vx_video** out_video, vx_video_info* out_video_info)
{
	vx_video* video = NULL;
	vx_error error = VX_ERR_UNKNOWN;

	if ((error = vx_open_internal(&video, filename, options)) != VX_ERR_SUCCESS)
		goto cleanup;

	if ((error = vx_get_properties(video, out_video_info)) != VX_ERR_SUCCESS)
		goto cleanup;

	vx_close(video);

	if ((error = vx_open_internal(&video, filename, options)) != VX_ERR_SUCCESS)
		goto cleanup;

	*out_video = video;

	return VX_ERR_SUCCESS;

cleanup:
	vx_close(video);
	return error;
}

void vx_close(vx_video* video)
{
	if (!video) {
		return;
	}

	if (video->filter_pipeline)
		avfilter_graph_free(&video->filter_pipeline);

	if (video->filter_pipeline_audio)
		avfilter_graph_free(&video->filter_pipeline_audio);

	if (video->swr_ctx)
		swr_free(&video->swr_ctx);

	if (video->fmt_ctx)
		avformat_close_input(&video->fmt_ctx);

	for (int i = 0; i < video->frame_queue_count; i++) {
		av_frame_unref(video->frame_queue[i]);
		av_frame_free(&video->frame_queue[i]);
	}

	if (video->hw_device_ctx)
		av_buffer_unref(&video->hw_device_ctx);

	if (video->audio_codec_ctx)
        avcodec_free_context(&video->audio_codec_ctx);

    if (video->video_codec_ctx)
        avcodec_free_context(&video->video_codec_ctx);

	free(video);
}

static bool vx_read_packet(AVFormatContext* fmt_ctx, AVPacket* packet, int stream)
{
	// Try to read a frame, if it can't be read, skip ahead a bit and try again
	int64_t last_fp = avio_tell(fmt_ctx->pb);

	for (int i = 0; i < 1024; i++) {
		// The packet will be overwritten so it must be cleared first
		if (packet && packet->data)
			av_packet_unref(packet);

		int ret = av_read_frame(fmt_ctx, packet);

		// Success
		if (ret == 0)
			return true;

		// End of file, no need to retry
		if (ret == AVERROR_EOF || avio_feof(fmt_ctx->pb))
			return false;

		// Other error, might be a damaged stream, seek forward a couple bytes and try again
		if ((i % 10) == 0) {
			int64_t fp = avio_tell(fmt_ctx->pb);

			if (fp <= last_fp)
				fp = last_fp + 100 * (int64_t)i;

			av_log(NULL, AV_LOG_DEBUG, "Seeking forward in stream, position: @%" PRId64 "\n", fp);
			avformat_seek_file(fmt_ctx, stream, fp + 100, fp + 512, fp + 1024 * 1024, AVSEEK_FLAG_BYTE | AVSEEK_FLAG_ANY);

			last_fp = fp;
		}
	}

	return false;
}

vx_error vx_get_frame_rate(const vx_video* video, float* out_fps)
{
	AVRational rate = video->fmt_ctx->streams[video->video_stream]->avg_frame_rate;

	if (rate.num == 0 || rate.den == 0)
		return VX_ERR_FRAME_RATE;

	*out_fps = (float)av_q2d(rate);
	return VX_ERR_SUCCESS;
}

static bool vx_video_is_rotated(vx_video video)
{
	char* transform = "";
	char* transform_args = "";

	return video.options.autorotate
		&& vx_get_rotation_transform(video.fmt_ctx->streams[video.video_stream], &transform, &transform_args) == VX_ERR_SUCCESS
		&& strcmp(transform, "transpose") == 0
		&& (strcmp(transform_args, ROTATION_CLOCKWISE) == 0 || strcmp(transform_args, ROTATION_COUNTERCLOCKWISE) == 0);
}

/// <summary>
/// Attempt to calculate frame dimensions based on cropping and rotation options,
/// before the actual frame dimensions are known (i.e. after filtering)
/// </summary>
vx_error vx_get_adjusted_frame_dimensions(const vx_video* video, int* width, int* height)
{
	if (vx_rectangle_is_initialized(video->options.crop_area)) {
		*width = video->options.crop_area.width;
		*height = video->options.crop_area.height;
	}
	else if (vx_video_is_rotated(*video)) {
		int temp = *width;
		*width = *height;
		*height = temp;
	}

	return VX_ERR_SUCCESS;
}

/// <summary>
/// The original video width, before cropping but after auto rotation (if enabled)
/// </summary>
int vx_get_width(const vx_video* video)
{
	return vx_video_is_rotated(*video) ? video->video_codec_ctx->height : video->video_codec_ctx->width;
}

/// <summary>
/// The original video height, before cropping but after auto rotation (if enabled)
/// </summary>
int vx_get_height(const vx_video* video)
{
	return vx_video_is_rotated(*video) ? video->video_codec_ctx->width : video->video_codec_ctx->height;
}

/// <summary>
/// The video width, after cropping and auto rotation (if enabled)
/// </summary>
int vx_get_adjusted_width(const vx_video* video)
{
	int width = video->video_codec_ctx->width;
	int height = video->video_codec_ctx->height;
	vx_get_adjusted_frame_dimensions(video, &width, &height);

	return width;
}

/// <summary>
/// The video height, after cropping and auto rotation (if enabled)
/// </summary>
int vx_get_adjusted_height(const vx_video* video)
{
	int width = video->video_codec_ctx->width;
	int height = video->video_codec_ctx->height;
	vx_get_adjusted_frame_dimensions(video, &width, &height);

	return height;
}

double vx_estimate_timestamp(vx_video* video, const int stream_type, const int64_t pts)
{
	if (video->ts_offset == AV_NOPTS_VALUE && stream_type == video->video_stream)
		video->ts_offset = pts;

	double ts_estimated = 0.0;
	double ts_seconds = vx_timestamp_to_seconds(video, stream_type, pts - video->ts_offset);
	double ts_delta = ts_seconds - video->ts_last;

	// Not all codecs supply a timestamp, or they supply values that don't progress nicely
	// So sometimes we need to estimate based on FPS
	if (pts == AV_NOPTS_VALUE || ts_delta <= 0 || ts_delta >= 2) {
		// Initial timestamp should be zero
		if (video->ts_last < 0 || video->frame_count == 0) {
			video->ts_last = 0;
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
		video->ts_last = ts_seconds;
	}

	return stream_type == video->video_stream
		? video->ts_last += ts_estimated
		: video->ts_last + ts_estimated;
}

/// <summary>
/// Allocate a suitable frame audio buffer
/// </summary>
/// <returns>
/// The number of samples per channel that were allocated.
/// </returns>
static int vx_frame_init_audio_buffer(
	vx_frame* frame,
	const struct av_audio_params in_params,
	const struct vx_audio_params out_params,
	const int frame_size)
{
	if (frame->audio_buffer) {
		av_freep(&frame->audio_buffer[0]);
		av_freep(&frame->audio_buffer);
	}

	// The maximum number of samples per frame, each frame will usually contain far fewer than this
	int frame_samples_count = !frame_size || frame_size <= 0
		? in_params.sample_rate * 2 // Two seconds of buffer
		: frame_size;

	// The number of samples required per channel
	int sample_count = (int)av_rescale_rnd(frame_samples_count, out_params.sample_rate, in_params.sample_rate, AV_ROUND_UP);

	int ret = av_samples_alloc_array_and_samples(
		&frame->audio_buffer,
		NULL,
		out_params.channels,
		sample_count,
		vx_to_av_sample_fmt(out_params.sample_format),
		0);

	frame->max_samples = sample_count;

	return  ret >= 0
		? sample_count
		: -1;
}

vx_error vx_frame_init_buffer(vx_frame* frame)
{
	vx_error result = VX_ERR_ALLOCATE;
	int av_pixfmt = vx_to_av_pix_fmt(frame->pix_fmt);

	// Includes some padding as a workaround for a bug in swscale (?) where it overreads the buffer
	int buffer_size = av_image_get_buffer_size(av_pixfmt, frame->width, frame->height, 1) + FRAME_BUFFER_PADDING;

	if (buffer_size <= 0)
		return result;

	frame->buffer = av_mallocz(buffer_size);

	if (!frame->buffer)
		return result;

	return VX_ERR_SUCCESS;
}

vx_frame* vx_frame_create(const vx_video* video, int width, int height, vx_pix_fmt pix_fmt)
{
	vx_frame* frame = calloc(1, sizeof(vx_frame));

	if (!frame)
		goto error;

	frame->width = width;
	frame->height = height;
	frame->pix_fmt = pix_fmt;

	if (video->audio_codec_ctx && video->options.audio_params.channels > 0) {
		struct av_audio_params params = av_audio_params_from_codec(video->audio_codec_ctx);

		if (vx_frame_init_audio_buffer(frame, params, video->options.audio_params, video->audio_codec_ctx->frame_size) <= 0)
			goto error;
	}

	if (vx_frame_init_buffer(frame) != VX_ERR_SUCCESS)
		goto error;

	return frame;

error:
	if (frame)
		free(frame);

	return NULL;
}

void vx_frame_destroy(vx_frame* me)
{
	av_free(me->buffer);

	if (me->audio_buffer) {
		av_freep(&me->audio_buffer[0]);
		av_freep(&me->audio_buffer);
	}

	free(me);
}

/// <summary>
/// Make a best guess to whether a frame contains an image. Some codecs do not set all
/// frame properties so just checking the picture type is not sufficient
/// </summary>
bool vx_frame_has_image(const AVFrame* frame)
{
	return frame->pict_type != AV_PICTURE_TYPE_NONE || (frame->width > 0 && frame->height > 0 && frame->nb_samples == 0);
}

void* vx_frame_get_video_buffer(const vx_frame* frame, int* out_buffer_size)
{
	int av_pixfmt = vx_to_av_pix_fmt(frame->pix_fmt);
	*out_buffer_size = av_image_get_buffer_size(av_pixfmt, frame->width, frame->height, 1) + FRAME_BUFFER_PADDING;

	return frame->buffer;
}

void* vx_frame_get_audio_buffer(const vx_frame* frame, int* out_sample_count)
{
	*out_sample_count = frame->sample_count;

	return frame->audio_buffer
		? frame->audio_buffer[0]
		: NULL;
}

vx_audio_info vx_frame_get_audio_info(const vx_frame* frame)
{
	return frame->audio_info;
}

vx_scene_info vx_frame_get_scene_info(const vx_frame* frame)
{
	return frame->scene_info;
}

static vx_error vx_decode_next_packet(vx_video* me, AVPacket* packet, AVCodecContext** out_codec)
{
	vx_error ret = VX_ERR_SUCCESS;
	int result = 0;

	do {
		*out_codec = NULL;
		// Clear the packet for resuse
		if (packet && packet->data)
			av_packet_unref(packet);

		// Get a packet, which will usually be a single video frame, or several complete audio frames
		vx_read_packet(me->fmt_ctx, packet, me->video_stream);

		// Only attempt to decode packets from the streams that have been selected
		if (packet && packet->data && (packet->stream_index != me->video_stream && packet->stream_index != me->audio_stream)) {
			// Skip to the next packet
			result = -1;
			continue;
		}

		// The decoder may still hold a couple of cached frames, so even if the end of the file has been
		// reached and no packet is returned, it still needs to be sent in order to flush the decoder
		if (!packet || !packet->data) {
			*out_codec = me->video_codec_ctx;
		}
		else {
			*out_codec = packet->stream_index == me->video_stream
				? me->video_codec_ctx
				: me->audio_codec_ctx;
		}

		result = *out_codec != NULL
			? avcodec_send_packet(*out_codec, packet)
			: VX_ERR_FIND_CODEC;
	} while (!result == AVERROR_EOF || vx_is_packet_error(result));

	if (result == AVERROR_EOF) {
		ret = VX_ERR_EOF;
	}
	else if (vx_is_packet_error(result)) {
		av_log_error_message(result, AV_LOG_ERROR, "Unable to decode packet: %s\n");

		ret = VX_ERR_DECODE_VIDEO;
	}

	return ret;
}

static vx_error vx_decode_frame(vx_video* me, AVPacket* packet, AVFrame* out_frame_buffer[FRAME_QUEUE_SIZE], int* out_frame_count)
{
	vx_error ret = VX_ERR_UNKNOWN;
	AVCodecContext* codec = NULL;
	AVFrame* frame = NULL;
	int frame_count = 0;
	*out_frame_count = 0;
	int result = 0;

	if ((ret = vx_decode_next_packet(me, packet, &codec)) != VX_ERR_SUCCESS)
		goto cleanup;

	// Don't decode audio frames unless audio is enabled
	if (packet->stream_index == me->audio_stream && !me->swr_ctx) {
		ret = VX_ERR_SUCCESS;
		goto cleanup;
	}

	// Store all frames returned by the decoder
	while (result >= 0) {
		frame = av_frame_alloc();
		result = avcodec_receive_frame(codec, frame);

		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
			break;
		}
		else if (result < 0) {
			ret = VX_ERR_DECODE_VIDEO;
			goto cleanup;
		}
		else {
			if (frame_count < FRAME_QUEUE_SIZE) {
				out_frame_buffer[frame_count++] = frame;
			}
			else {
				// Dump the frame and the rest of the packet data to prevent buffer overrun
				av_log(NULL, AV_LOG_WARNING, "Unable to return all frames, temporary frame buffer full. Dropping excess frames\n");
				break;
			}
		}
	}

	*out_frame_count = frame_count;
	ret = VX_ERR_SUCCESS;

cleanup:
	if (frame) {
		av_frame_unref(frame);
		av_frame_free(&frame);
	}

	return ret;
}

static double vx_frame_metadata_as_double(const AVFrame* av_frame, const char* key, double defaultValue)
{
	double value = defaultValue;
	const AVDictionaryEntry* entry = av_dict_get(av_frame->metadata, key, NULL, AV_DICT_MATCH_CASE);

	if (entry) {
		sscanf(entry->value, "%lf", &value);
	}

	return value == INFINITY || value == -INFINITY
		? defaultValue
		: value;
}

static vx_error vx_frame_properties_from_metadata(vx_frame* frame, const AVFrame* av_frame)
{
	vx_audio_info audio_info = { 0 };
	vx_scene_info scene_info = { 0, 0, false };
	double audio_default = -100;

	audio_info.peak_level = vx_frame_metadata_as_double(av_frame, "lavfi.astats.Overall.Peak_level", audio_default);
	audio_info.rms_level = vx_frame_metadata_as_double(av_frame, "lavfi.astats.Overall.RMS_level", audio_default);
	audio_info.rms_peak = vx_frame_metadata_as_double(av_frame, "lavfi.astats.Overall.RMS_peak", audio_default);

	scene_info.difference = vx_frame_metadata_as_double(av_frame, "lavfi.scd.mafd", 0);
	scene_info.scene_score = vx_frame_metadata_as_double(av_frame, "lavfi.scd.score", 0);
	scene_info.new_scene = av_dict_get(av_frame->metadata, "lavfi.scd.time", NULL, AV_DICT_MATCH_CASE) != NULL;

	frame->audio_info = audio_info;
	frame->scene_info = scene_info;

	return VX_ERR_SUCCESS;
}

static vx_error vx_scale_frame(const AVFrame* av_frame, vx_frame* frame)
{
	vx_error ret = VX_ERR_UNKNOWN;
	int av_pixfmt = vx_to_av_pix_fmt(frame->pix_fmt);

	struct SwsContext* sws_ctx = sws_getContext(
		av_frame->width, av_frame->height, av_frame->format,
		frame->width, frame->height, av_pixfmt,
		SWS_FAST_BILINEAR, NULL, NULL, NULL);

	if (!sws_ctx) {
		ret = VX_ERR_SCALING;
		goto cleanup;
	}

	assert(av_frame->data);

	int fmtBytesPerPixel[3] = { 3, 1, 4 };

	uint8_t* pixels[3] = { frame->buffer, 0, 0 };
	int pitch[3] = { fmtBytesPerPixel[frame->pix_fmt] * frame->width, 0, 0 };

	sws_scale(sws_ctx, (const uint8_t* const*)av_frame->data, av_frame->linesize, 0, av_frame->height, pixels, pitch);

	sws_freeContext(sws_ctx);

	return VX_ERR_SUCCESS;

cleanup:
	return ret;
}

static vx_error vx_frame_process_audio(vx_video* video, AVFrame* av_frame, vx_frame* frame)
{
	if (!video->audio_codec_ctx || !video->swr_ctx) {
		return VX_ERR_DECODE_AUDIO;
	}

	struct av_audio_params initial_params = video->inital_audio_params;
	struct av_audio_params params = av_audio_params_from_frame(av_frame, &video->fmt_ctx->streams[video->audio_stream]->time_base);
	vx_audio_params out_params = video->options.audio_params;

	int estimated_sample_count = (int)av_rescale_rnd(av_frame->nb_samples, out_params.sample_rate, params.sample_rate, AV_ROUND_UP);

	if (!av_audio_params_equal(initial_params, params))
	{
		av_log(NULL, AV_LOG_INFO, "Audio format changed\n");
		av_log(NULL, AV_LOG_INFO, "Channels:\t\t%d -> %d\n", initial_params.channel_layout.nb_channels, params.channel_layout.nb_channels);
		av_log(NULL, AV_LOG_INFO, "Channel layout:\t%d -> %d\n", initial_params.channel_layout.order, params.channel_layout.order);
		av_log(NULL, AV_LOG_INFO, "Sample rate:\t\t%d -> %d\n", initial_params.sample_rate, params.sample_rate);
		av_log(NULL, AV_LOG_INFO, "Sample format:\t%d -> %d\n", initial_params.sample_format, params.sample_format);
		av_log(NULL, AV_LOG_INFO, "Time base:\t\t%d/%d -> %d/%d\n",
			initial_params.time_base.num, initial_params.time_base.den, params.time_base.num, params.time_base.den);

		// Reinitialize resampler if audio format changes mid stream
		if (vx_frame_init_audio_buffer(frame, params, video->options.audio_params, NULL) <= 0)
			return VX_ERR_RESAMPLE_AUDIO;
		if (vx_init_audio_resampler(video, params, out_params) != VX_ERR_SUCCESS) {
			av_log(NULL, AV_LOG_ERROR, "Unable to reinitialize audio resampler after format change\n");
			return VX_ERR_ALLOCATE;
		}
	}

	// Expand the frame audio buffer in case a frame has an unexpectedly high number of samples
	if (frame->max_samples <= 0 || frame->max_samples < estimated_sample_count)
		if (vx_frame_init_audio_buffer(frame, params, video->options.audio_params, av_frame->nb_samples) <= 0)
			return VX_ERR_RESAMPLE_AUDIO;

	int sample_count = swr_convert(video->swr_ctx, frame->audio_buffer, estimated_sample_count, (const uint8_t**)av_frame->data, av_frame->nb_samples);

	if (sample_count < 0) {
		return VX_ERR_RESAMPLE_AUDIO;
	}

	frame->sample_count = sample_count;

	return VX_ERR_SUCCESS;
}

vx_error vx_queue_frames(vx_video* me)
{
	vx_error ret = VX_ERR_SUCCESS;
	AVPacket* packet = NULL;
	static AVFrame* frame_buffer[FRAME_QUEUE_SIZE] = { NULL };
	int frame_idx = 0;
	int frame_count = 0;

	// Only top up the queue in chunks instead of for every single frame
	if (me->frame_queue_count > FRAME_QUEUE_SIZE / 4) {
		return VX_ERR_SUCCESS;
	}

	packet = av_packet_alloc();
	if (!packet) {
		ret = VX_ERR_ALLOCATE;
		goto cleanup;
	}

	// Leave some room in the frame queue to buffer the return from the decoder
	while (ret == VX_ERR_SUCCESS && me->frame_queue_count < FRAME_QUEUE_SIZE / 2) {
		ret = vx_decode_frame(me, packet, &frame_buffer, &frame_count);

		if (ret != VX_ERR_SUCCESS && frame_count <= 0)
			goto cleanup;

		// The decoder usually only returns a single video frame, but there may be several audio frames
		for (int i = 0; i < frame_count; i++) {
			if (me->frame_queue_count < FRAME_QUEUE_SIZE) {
				vx_enqueue(me, frame_buffer[i]);
			}
			else {
				ret = VX_ERR_UNKNOWN;
				frame_idx = i;
				goto cleanup;
			}
		}
	}

cleanup:
	if (ret != VX_ERR_SUCCESS) {
		for (int i = frame_idx; i < frame_count; i++) {
			AVFrame* frame = frame_buffer[i];
			if (frame) {
				av_frame_unref(frame);
				av_frame_free(&frame);
			}
		}
	}

	if (packet && packet->data)
		av_packet_unref(packet);
	av_packet_free(&packet);

	return ret;
}

vx_error vx_frame_step_internal(vx_video* me, vx_frame_info* frame_info)
{
	vx_error ret = VX_ERR_UNKNOWN;
	AVFrame* frame = NULL;

	// Free the first item in the queue (if any)
	if (me->frame_queue_count > 0) {
		frame = vx_dequeue(me);

		if (!frame) {
			return VX_ERR_UNKNOWN;
		}

		av_frame_unref(frame);
		av_frame_free(&frame);
	}

	// (Re)fill the frame queue
	ret = vx_queue_frames(me);

	if (me->frame_queue_count > 0) {
		frame = vx_get_first_queue_item(me);

		// Check for audio before defaulting to video. Some codecs fail to write the picture type to frames
		int stream_type = frame->pict_type == AV_PICTURE_TYPE_NONE && frame->nb_samples > 0
			? me->audio_stream
			: me->video_stream;

		double ts = frame->best_effort_timestamp != AV_NOPTS_VALUE && frame->best_effort_timestamp > 0
			? vx_timestamp_to_seconds(me, stream_type, frame->best_effort_timestamp)
			: 0;

		// Have to return the calculated frame dimensions here. The dimensions are
		// needed before they are actually known, i.e. after filtering
		frame_info->width = frame->width;
		frame_info->height = frame->height;
		vx_get_adjusted_frame_dimensions(me, &frame_info->width, &frame_info->height);
		// TODO: Handle duplicate audio timestamps
		frame_info->timestamp = vx_estimate_timestamp(me, stream_type, frame->best_effort_timestamp);
		frame_info->flags = vx_frame_has_image(frame) ? VX_FF_HAS_IMAGE : 0;
		frame_info->flags |= frame->nb_samples > 0 ? VX_FF_HAS_AUDIO : 0;
		frame_info->flags |= frame->pict_type == AV_PICTURE_TYPE_I ? VX_FF_KEYFRAME : 0;
		if (frame->pkt_pos != -1)
			frame_info->flags |= frame->pkt_pos < 0 ? VX_FF_BYTE_POS_GUESSED : 0;
		frame_info->flags |= frame->pts > 0 ? VX_FF_HAS_PTS : 0;

		// Override errors that may be returned when queuing frames until
		// the frame queue is processed
		ret = VX_ERR_SUCCESS;
	}

	return ret;
}

vx_error vx_frame_step(vx_video* me, vx_frame_info* out_frame_info)
{
	vx_error first_error = VX_ERR_SUCCESS;
	const int retry_count = 100;

	for (int i = 0; i < retry_count; i++)
	{
		vx_error e = vx_frame_step_internal(me, out_frame_info);

		if (!(e == VX_ERR_UNKNOWN || e == VX_ERR_VIDEO_STREAM || e == VX_ERR_DECODE_VIDEO ||
			e == VX_ERR_DECODE_AUDIO || e == VX_ERR_NO_AUDIO || e == VX_ERR_RESAMPLE_AUDIO))
		{
			if (out_frame_info->flags & VX_FF_HAS_IMAGE) {
				me->frame_count++;
			}

			return e;
		}

		first_error = first_error != VX_ERR_SUCCESS ? first_error : e;
	}

	return first_error;
}

vx_error vx_frame_transfer_audio_data(const vx_video* video, AVFrame* av_frame, vx_frame* frame)
{
	vx_error result = VX_ERR_SUCCESS;

	result = vx_filter_frame(video, av_frame, AVMEDIA_TYPE_AUDIO);

	// Fill the audio buffer
	if (result == VX_ERR_SUCCESS)
		result = vx_frame_process_audio(video, av_frame, frame);

	return result;
}

vx_error vx_frame_transfer_video_data(const vx_video* video, AVFrame* av_frame, vx_frame* frame)
{
	vx_error result = VX_ERR_UNKNOWN;
	AVFrame* hw_frame = NULL;

	// Copy the frame from GPU memory if it has been hardware decoded
	if (av_frame->hw_frames_ctx)
	{
		hw_frame = av_frame_alloc();

		if (!hw_frame) {
			result = VX_ERR_ALLOCATE;
			goto cleanup;
		}

		// Transfer the hardware frame to a temporary frame
		av_frame_move_ref(hw_frame, av_frame);

		// Copy the data and properties from the temporary hardware frame
		if (av_hwframe_transfer_data(av_frame, hw_frame, 0) < 0 | av_frame_copy_props(av_frame, hw_frame) < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Error transferring frame data to system memory\n");
			goto cleanup;
		}
	}

	if (vx_filter_frame(video, av_frame, AVMEDIA_TYPE_VIDEO) != VX_ERR_SUCCESS) {
		goto cleanup;
	}

	// The frame dimensions may have changed since it was initialized
	if (frame->width != av_frame->width || frame->height != av_frame->height) {
		av_free(frame->buffer);

		frame->width = av_frame->width;
		frame->height = av_frame->height;

		if (vx_frame_init_buffer(frame) != VX_ERR_SUCCESS)
			goto cleanup;
	}

	// Fill the buffer
	if (vx_scale_frame(av_frame, frame) != VX_ERR_SUCCESS) {
		goto cleanup;
	}

	result = VX_ERR_SUCCESS;

cleanup:
	if (hw_frame) {
		av_frame_unref(hw_frame);
		av_frame_free(&hw_frame);
	}

	return result;
}

vx_error vx_frame_transfer_data(const vx_video* video, vx_frame* frame)
{
	vx_error ret = VX_ERR_SUCCESS;
	AVFrame* av_frame = NULL;

	if (video->frame_queue_count <= 0)
		return VX_ERR_EOF;

	// Get the first item from the queue, but do not dequeue
	av_frame = vx_get_first_queue_item(video);
	if (!av_frame)
	{
		ret = VX_ERR_UNKNOWN;
		goto cleanup;
	}

	// Run the frame through the filter pipeline, if any
	if (vx_frame_has_image(av_frame)) {
		if ((ret = vx_frame_transfer_video_data(video, av_frame, frame)) != VX_ERR_SUCCESS) {
			goto cleanup;
		}
	}
	else if (av_frame->nb_samples > 0) {
		if ((ret = vx_frame_transfer_audio_data(video, av_frame, frame)) != VX_ERR_SUCCESS) {
			goto cleanup;
		}
	}
	else {
		av_log(NULL, AV_LOG_WARNING, "A frame did not contain either audio or video data so the buffer could not be transferred\n");
	}

	// Frame properties that may have been updated after filtering
	if ((ret = vx_frame_properties_from_metadata(frame, av_frame)) != VX_ERR_SUCCESS) {
		goto cleanup;
	}

cleanup:
	return ret;
}

bool vx_get_hw_context_present(const vx_video* video)
{
	return video->hw_device_ctx != NULL;
}