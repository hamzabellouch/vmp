#include "thumbnail_generator.h"
#include <iostream>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

ThumbnailGenerator::ThumbnailGenerator() {}

ThumbnailGenerator::~ThumbnailGenerator() {
    stop();
}

void ThumbnailGenerator::stop() {
    is_running = false;
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
    std::lock_guard<std::mutex> lock(thumb_mutex);
    cached_thumbnails.clear();
}

void ThumbnailGenerator::start_generation(const std::string& filepath, double duration_sec) {
    stop();

    if (filepath.empty() || duration_sec <= 2.0) return;

    current_filepath = filepath;
    duration = duration_sec;
    is_running = true;

    worker_thread = std::thread(&ThumbnailGenerator::generate_loop, this);
}

void ThumbnailGenerator::generate_loop() {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, current_filepath.c_str(), NULL, NULL) < 0) {
        return;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return;
    }

    const AVCodec* decoder = nullptr;
    int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (video_stream_idx < 0) {
        avformat_close_input(&fmt_ctx);
        return;
    }

    AVStream* st = fmt_ctx->streams[video_stream_idx];
    AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(codec_ctx, st->codecpar);
    codec_ctx->thread_count = 1; // 1 thread is enough for background thumbnail downscaling
    
    if (avcodec_open2(codec_ctx, decoder, NULL) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }

    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();

    rgb_frame->format = AV_PIX_FMT_RGB24;
    rgb_frame->width = THUMB_W;
    rgb_frame->height = THUMB_H;
    av_frame_get_buffer(rgb_frame, 32);

    int num_samples = std::min(NUM_SLICES, static_cast<int>(std::max(10.0, duration / 5.0)));
    double interval = duration / (num_samples + 1);

    for (int i = 1; i <= num_samples && is_running; i++) {
        double target_sec = i * interval;
        int64_t target_pts = av_rescale_q(static_cast<int64_t>(target_sec * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
        
        av_seek_frame(fmt_ctx, video_stream_idx, target_pts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec_ctx);

        bool frame_found = false;
        int attempts = 0;

        while (!frame_found && attempts < 30 && is_running) {
            attempts++;
            if (av_read_frame(fmt_ctx, packet) < 0) break;

            if (packet->stream_index == video_stream_idx) {
                if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                    if (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                        double actual_pts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * av_q2d(st->time_base) : target_sec;

                        sws_ctx = sws_getCachedContext(
                            sws_ctx,
                            frame->width, frame->height, (AVPixelFormat)frame->format,
                            THUMB_W, THUMB_H, AV_PIX_FMT_RGB24,
                            SWS_FAST_BILINEAR, NULL, NULL, NULL
                        );

                        if (sws_ctx) {
                            sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
                                      rgb_frame->data, rgb_frame->linesize);

                            ThumbnailItem item;
                            item.pts = actual_pts;
                            item.width = THUMB_W;
                            item.height = THUMB_H;
                            item.rgb_data.resize(THUMB_W * THUMB_H * 3);

                            for (int y = 0; y < THUMB_H; y++) {
                                memcpy(item.rgb_data.data() + y * THUMB_W * 3,
                                       rgb_frame->data[0] + y * rgb_frame->linesize[0],
                                       THUMB_W * 3);
                            }

                            {
                                std::lock_guard<std::mutex> lock(thumb_mutex);
                                cached_thumbnails.push_back(item);
                                // Sort by timestamp
                                std::sort(cached_thumbnails.begin(), cached_thumbnails.end(),
                                          [](const ThumbnailItem& a, const ThumbnailItem& b) {
                                              return a.pts < b.pts;
                                          });
                            }
                            frame_found = true;
                        }
                    }
                }
            }
            av_packet_unref(packet);
        }
        
        // Small throttle sleep between thumbnail extractions to preserve CPU cycles
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (sws_ctx) sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
}

bool ThumbnailGenerator::has_thumbnails() const {
    std::lock_guard<std::mutex> lock(thumb_mutex);
    return !cached_thumbnails.empty();
}

bool ThumbnailGenerator::get_thumbnail_at_time(double target_sec, std::vector<uint8_t>& out_rgb, int& out_w, int& out_h) {
    std::lock_guard<std::mutex> lock(thumb_mutex);
    if (cached_thumbnails.empty()) return false;

    // Binary search for closest timestamp
    auto it = std::lower_bound(cached_thumbnails.begin(), cached_thumbnails.end(), target_sec,
                               [](const ThumbnailItem& item, double val) {
                                   return item.pts < val;
                               });

    const ThumbnailItem* best = nullptr;
    if (it == cached_thumbnails.begin()) {
        best = &(*it);
    } else if (it == cached_thumbnails.end()) {
        best = &cached_thumbnails.back();
    } else {
        auto prev = it - 1;
        if (std::abs(it->pts - target_sec) < std::abs(prev->pts - target_sec)) {
            best = &(*it);
        } else {
            best = &(*prev);
        }
    }

    if (best && !best->rgb_data.empty()) {
        out_rgb = best->rgb_data;
        out_w = best->width;
        out_h = best->height;
        return true;
    }
    return false;
}
