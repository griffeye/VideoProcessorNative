#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>

#include "libvx.h"

int main(int argc, char* argv[]) {
    int result = 1;
    char* video_path = argc > 1 ? argv[1] : "";
    vx_video* video = NULL;
    vx_video_info* video_info = calloc(1, sizeof(vx_video_info));
    vx_frame_info* frame_info = calloc(1, sizeof(vx_frame_info));
    vx_frame* frame = NULL;

    vx_audio_params audio_params = {
        .channels = 2,
        .sample_format = VX_SAMPLE_FMT_FLT,
        .sample_rate = 44100
    };
    const vx_video_options options = {
        .audio_params = audio_params,
        .autorotate = true,
        .crop_area = {0},
        .hw_criteria = VX_HW_ACCEL_ALL,
        .scene_threshold = 0.5
    };

    if (vx_open(video_path, options, &video, video_info) != VX_ERR_SUCCESS) {
        printf("Failed to open video: %s\n", video_path);
        goto cleanup;
    }

    // Initialize an empty frame that will be reused throughout the video
    frame = vx_frame_create(video, video_info->width, video_info->height, VX_PIX_FMT_RGB32);
    if (!frame) {
        printf("Failed to create frame\n");
        goto cleanup;
    }

    // Iterate over all frames in the video. The frame is decoded when stepping
    while (vx_frame_step(video, frame_info) == VX_ERR_SUCCESS) {
        bool is_video_frame = frame_info->flags & VX_FF_HAS_IMAGE;

        printf("Frame: %ld, %dx%d\n", video->frame_count, frame_info->width, frame_info->height);

        // Perform processing and tranfer from GPU to CPU if necessary
        if (vx_frame_transfer_data(video, frame) != VX_ERR_SUCCESS) {
            printf("Failed to transfer frame data\n");
            goto cleanup;
        }
        else {
            printf("Frame type: %s\n", is_video_frame ? "video" : "audio");
            // The frame image could be accessed via frame->buffer
            // Alternatively for audio frames, the audio samples could be accessed via frame->audio_data
        }
    }

    result = 0;

cleanup:
    vx_close(video);
    if (video_info) {
        free(video_info);
    }
    if (frame) {
        vx_frame_destroy(frame);
    }
    if (frame_info) {
        free(frame_info);
    }

    return result;
}