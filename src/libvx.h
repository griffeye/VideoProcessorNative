#ifndef LIBVX_H
#define LIBVX_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
//  Microsoft
#define VX_DECLSPEC __declspec(dllexport)
#define VX_CDECL __cdecl
#else
//  GCC
#define VX_DECLSPEC __attribute__((visibility("default")))
#define VX_CDECL
#endif

#define FRAME_QUEUE_SIZE 32

typedef struct vx_video vx_video;
typedef struct vx_audio_params vx_audio_params;
typedef struct vx_video_options vx_video_options;
typedef struct vx_audio_info vx_audio_info;
typedef struct vx_scene_info vx_scene_info;
typedef struct vx_frame vx_frame;
typedef struct vx_frame_info vx_frame_info;
typedef struct vx_rectangle vx_rectangle;
typedef struct vx_video_info vx_video_info;

typedef enum {
	VX_LOG_NONE				= 0,
	VX_LOG_FATAL			= 1,
	VX_LOG_ERROR			= 2,
	VX_LOG_WARNING			= 3,
	VX_LOG_INFO				= 4,
	VX_LOG_DEBUG			= 5
} vx_log_level;

typedef enum {
	VX_PIX_FMT_RGB24		= 0,
	VX_PIX_FMT_GRAY8		= 1,
	VX_PIX_FMT_RGB32		= 2,
} vx_pix_fmt;

typedef enum {
	VX_SAMPLE_FMT_S16		= 0,
	VX_SAMPLE_FMT_FLT		= 1
} vx_sample_fmt;

typedef enum {
	VX_ERR_SUCCESS			 = 0,
	VX_ERR_UNKNOWN			 = 1,
	VX_ERR_ALLOCATE			 = 2,
	VX_ERR_FRAME_RATE		 = 3,
	VX_ERR_OPEN_FILE		 = 4,
	VX_ERR_STREAM_INFO		 = 5,
	VX_ERR_VIDEO_STREAM		 = 6,
	VX_ERR_FIND_CODEC		 = 7,
	VX_ERR_OPEN_CODEC		 = 8,
	VX_ERR_EOF				 = 9,
	VX_ERR_DECODE_VIDEO		 = 10,
	VX_ERR_SCALING			 = 11,
	VX_ERR_PIXEL_ASPECT		 = 12,
	VX_ERR_DECODE_AUDIO		 = 13,
	VX_ERR_NO_AUDIO			 = 14,
	VX_ERR_RESAMPLE_AUDIO	 = 15,
	VX_ERR_FILE_NOT_FOUND	 = 16,
	VX_ERR_INIT_FILTER		 = 17,
	VX_ERR_OPEN_INVALID_CROP = 18
} vx_error;

typedef enum {
	VX_FF_HAS_IMAGE			= 1 << 0,
	VX_FF_HAS_AUDIO			= 1 << 1,
	VX_FF_KEYFRAME			= 1 << 2,
	VX_FF_BYTE_POS_GUESSED	= 1 << 3,
	VX_FF_HAS_PTS			= 1 << 4,
	VX_FF_DEFERRED			= 1 << 5
} vx_frame_flag;

typedef enum {
	VX_HW_ACCEL_ALL			= 1 << 0,
	VX_HW_ACCEL_720			= 1 << 1,
	VX_HW_ACCEL_1080		= 1 << 2,
	VX_HW_ACCEL_1440		= 1 << 3,
	VX_HW_ACCEL_2160		= 1 << 4,
	VX_HW_ACCEL_HEVC		= 1 << 5,
	VX_HW_ACCEL_H264		= 1 << 6
} vx_hwaccel_flag;

struct vx_rectangle
{
	int x;
	int y;
	int width;
	int height;
};

struct vx_audio_params
{
	int channels;
	vx_sample_fmt sample_format;
	int sample_rate;
};

struct av_video_params
{
	int width;
	int height;
	AVRational sample_aspect_ratio;
	enum AVPixelFormat pixel_format;
	AVRational time_base;
};

struct av_audio_params
{
	AVChannelLayout channel_layout;
	enum AVSampleFormat sample_format;
	int sample_rate;
	AVRational time_base;
};

struct vx_audio_info
{
	double peak_level;
	double rms_level;
	double rms_peak;
};

struct vx_scene_info
{
	double difference;
	double scene_score;
	bool new_scene;
};

struct vx_frame_info
{
	int width;
	int height;
	double timestamp;
	vx_frame_flag flags;
};

struct vx_frame
{
	int width;
	int height;
	vx_pix_fmt pix_fmt;
	int sample_count;
	int max_samples;

	vx_audio_info audio_info;
	vx_scene_info scene_info;

	uint8_t** audio_buffer;
	void* buffer;
};

struct vx_video_options
{
	vx_audio_params audio_params;
	bool autorotate;
	vx_rectangle crop_area;
	vx_hwaccel_flag hw_criteria;
	float scene_threshold;
};

struct vx_video
{
	AVFormatContext* fmt_ctx;
	AVCodecContext* video_codec_ctx;
	AVCodecContext* audio_codec_ctx;
	AVBufferRef* hw_device_ctx;

	struct SwrContext* swr_ctx;

	enum AVPixelFormat hw_pix_fmt;
	struct av_audio_params inital_audio_params;

	struct AVFilterGraph* filter_pipeline;
	struct AVFilterGraph* filter_pipeline_audio;

	int video_stream;
	int audio_stream;

	long frame_count;
	int frame_queue_count;
	AVFrame* frame_queue[FRAME_QUEUE_SIZE];

	vx_video_options options;

	double ts_last;
	int64_t ts_offset;
};

struct vx_video_info
{
	int width;
	int height;
	int adjusted_width;
	int adjusted_height;
	float frame_rate;
	int frame_count;
	float duration;
	bool has_audio;
	int audio_sample_rate;
	int audio_channels;
};

typedef void (*vx_log_callback)(const char* message, int level);

VX_DECLSPEC void VX_CDECL vx_log_set_cb(vx_log_callback cb);

VX_DECLSPEC vx_error VX_CDECL vx_open(const char* filename, const vx_video_options options, vx_video** out_video, vx_video_info* out_video_info);
VX_DECLSPEC void VX_CDECL vx_close(vx_video* video);

VX_DECLSPEC int VX_CDECL vx_get_width(const vx_video* video);
VX_DECLSPEC int VX_CDECL vx_get_height(const vx_video* video);
VX_DECLSPEC int VX_CDECL vx_get_adjusted_width(const vx_video* video);
VX_DECLSPEC int VX_CDECL vx_get_adjusted_height(const vx_video* video);
VX_DECLSPEC bool VX_CDECL vx_get_hw_context_present(const vx_video* video);

VX_DECLSPEC vx_frame* VX_CDECL vx_frame_create(const vx_video* video, int width, int height, vx_pix_fmt pix_fmt);
VX_DECLSPEC void VX_CDECL vx_frame_destroy(vx_frame* frame);

VX_DECLSPEC vx_error VX_CDECL vx_frame_step(vx_video* video, vx_frame_info* frame_info);
VX_DECLSPEC vx_error VX_CDECL vx_frame_transfer_data(const vx_video* video, vx_frame* frame);
VX_DECLSPEC void* VX_CDECL vx_frame_get_video_buffer(const vx_frame* frame, int* out_buffer_size);
VX_DECLSPEC void* VX_CDECL vx_frame_get_audio_buffer(const vx_frame* frame, int* out_sample_count);
VX_DECLSPEC vx_audio_info VX_CDECL vx_frame_get_audio_info(const vx_frame* frame);
VX_DECLSPEC vx_scene_info VX_CDECL vx_frame_get_scene_info(const vx_frame* frame);

#ifdef __cplusplus
}
#endif

#endif
