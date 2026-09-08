#include "video_player.h"
#include <cmath>

#include <unordered_set>

extern "C" {
#include <libavutil/imgutils.h>
}

VideoPlayer::VideoPlayer() {
    current_frame = av_frame_alloc();
    sw_frame = av_frame_alloc();
    converted_frame = av_frame_alloc();
    audio_frame = av_frame_alloc();
}

VideoPlayer::~VideoPlayer() {
    close_file();
    if (current_frame) av_frame_free(&current_frame);
    if (sw_frame) av_frame_free(&sw_frame);
    if (converted_frame) av_frame_free(&converted_frame);
    if (audio_frame) av_frame_free(&audio_frame);
}

void VideoPlayer::close_file() {
    stop();
    flush_frame_queue();
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
    }
    if (video_codec_ctx) {
        avcodec_free_context(&video_codec_ctx);
        video_codec_ctx = nullptr;
    }
    if (audio_codec_ctx) {
        avcodec_free_context(&audio_codec_ctx);
        audio_codec_ctx = nullptr;
    }
    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
        fmt_ctx = nullptr;
    }
    audio_engine.close();
    video_stream_index = -1;
    audio_stream_index = -1;
    duration_sec = 0.0;
    current_pts_sec = 0.0;
    stats = VMPStats();
    has_new_frame = false;
    audio_tracks.clear();
    current_audio_track_index = -1;
    video_clock_initialized = false;
    video_clock_base_pts = 0.0;
    paused = false;
}

void VideoPlayer::flush_frame_queue() {
    std::lock_guard<std::mutex> lock(frame_mutex);
    while (!frame_queue.empty()) {
        AVFrame* f = frame_queue.front().frame;
        if (f) av_frame_free(&f);
        frame_queue.pop();
    }
}

bool VideoPlayer::open_file(const std::string& filepath) {
    std::cout << "\n[VMP Engine] Opening media file: " << filepath << std::endl;
    close_file();

    // Check extension
    std::string ext = "";
    size_t dot_pos = filepath.find_last_of('.');
    if (dot_pos != std::string::npos) {
        ext = filepath.substr(dot_pos);
        for (char& c : ext) c = std::tolower(c);
    }

    static const std::unordered_set<std::string> image_extensions = {
        ".jpg", ".jpeg", ".png", ".bmp", ".svg", ".webp", ".gif", ".tiff", ".tif", ".ico", ".jfif", ".avif", ".heic"
    };
    if (image_extensions.count(ext) > 0) {
        std::cerr << "[VMP Error] Image files are not supported: " << filepath 
                  << " (VMP is dedicated to video playback only)" << std::endl;
        return false;
    }

    int err = avformat_open_input(&fmt_ctx, filepath.c_str(), NULL, NULL);
    if (err < 0) {
        char errbuf[256];
        av_strerror(err, errbuf, sizeof(errbuf));
        std::cerr << "[VMP Error] Could not open media file: " << filepath << " (" << errbuf << ")" << std::endl;
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        std::cerr << "[VMP Error] Could not retrieve stream info." << std::endl;
        return false;
    }

    if (fmt_ctx->iformat && fmt_ctx->iformat->name) {
        std::string ifmt = fmt_ctx->iformat->name;
        if (ifmt == "image2" || ifmt == "png_pipe" || ifmt == "jpeg_pipe" || ifmt == "svg_pipe" ||
            ifmt.find("image") != std::string::npos) {
            std::cerr << "[VMP Error] Image format detected (" << ifmt << "): " << filepath 
                      << " (VMP is dedicated to video playback only)" << std::endl;
            avformat_close_input(&fmt_ctx);
            return false;
        }
    }

    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration_sec = static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE;
    } else {
        duration_sec = 0.0;
    }

    // 1. Video Stream Setup
    const AVCodec* v_decoder = nullptr;
    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &v_decoder, 0);
    if (video_stream_index < 0) {
        std::cerr << "[VMP Error] No video stream found in file: " << filepath 
                  << " (VMP is dedicated to video playback only)" << std::endl;
        avformat_close_input(&fmt_ctx);
        return false;
    }

    if (video_stream_index >= 0) {
        AVStream* video_stream = fmt_ctx->streams[video_stream_index];
        video_codec_ctx = avcodec_alloc_context3(v_decoder);
        avcodec_parameters_to_context(video_codec_ctx, video_stream->codecpar);

        video_codec_ctx->thread_count = 0;
        video_codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        // Determine acceleration mode
        const char* force_cpu_env = std::getenv("VMP_FORCE_CPU");
        const char* hw_accel_env = std::getenv("VMP_HW_ACCEL");
        if (force_cpu_env && std::string(force_cpu_env) == "1") {
            hw_accel_mode = HWAccelMode::FORCE_CPU;
        } else if (hw_accel_env) {
            std::string env_str = hw_accel_env;
            if (env_str == "cpu" || env_str == "none" || env_str == "0" || env_str == "off") {
                hw_accel_mode = HWAccelMode::FORCE_CPU;
            } else if (env_str == "vaapi" || env_str == "cuda" || env_str == "force" || env_str == "hw") {
                hw_accel_mode = HWAccelMode::FORCE_HW;
            }
        }

        // Auto Heuristic: On Linux with VA-API copy-back, 4K+ (width >= 3840 || height >= 2160) VP9 / AV1
        // surface transfers (vaGetImage) are bandwidth-capped at ~30 FPS due to uncached bus memory readback.
        // Multi-threaded AVX2 CPU decoding delivers 100+ FPS at 4K. Prefer CPU engine for 4K+ VP9/AV1.
        bool is_4k_vp9_av1 = (video_codec_ctx->width >= 3840 || video_codec_ctx->height >= 2160) &&
                             (v_decoder->id == AV_CODEC_ID_VP9 || v_decoder->id == AV_CODEC_ID_AV1);

        bool try_hw = (hw_accel_mode == HWAccelMode::FORCE_HW) ||
                      (hw_accel_mode == HWAccelMode::AUTO && !is_4k_vp9_av1);

        bool hw_success = false;
        if (try_hw) {
            auto hw_devices = hw_decoder.get_supported_hw_devices();
            for (const auto& dev_name : hw_devices) {
                enum AVHWDeviceType type = av_hwdevice_find_type_by_name(dev_name.c_str());
                if (type != AV_HWDEVICE_TYPE_NONE) {
                    if (hw_decoder.init_hardware_context(video_codec_ctx, type)) {
                        stats.hw_acceleration_status = "ENABLED (" + dev_name + ")";
                        hw_success = true;
                        hw_accel_active = true;
                        break;
                    }
                }
            }
        }

        if (!hw_success) {
            hw_accel_active = false;
            stats.hw_acceleration_status = "DISABLED (Multi-Threaded CPU Engine)";
        }

        avcodec_open2(video_codec_ctx, v_decoder, NULL);
        stats.width = video_codec_ctx->width;
        stats.height = video_codec_ctx->height;
        stats.codec_name = v_decoder->name;

        // Determine stream frame rate accurately
        double fps = 0.0;
        if (video_stream->avg_frame_rate.den > 0 && video_stream->avg_frame_rate.num > 0) {
            fps = av_q2d(video_stream->avg_frame_rate);
        } else if (video_stream->r_frame_rate.den > 0 && video_stream->r_frame_rate.num > 0) {
            fps = av_q2d(video_stream->r_frame_rate);
        }
        if (fps <= 0.0 || fps > 300.0) fps = 24.0;
        stream_fps = fps;
        stats.stream_fps = fps;
        stats.fps = fps;
    }

    // 2. Audio Stream Setup
    const AVCodec* a_decoder = nullptr;
    audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &a_decoder, 0);
    
    // Scan all audio streams and record their indices
    audio_tracks.clear();
    current_audio_track_index = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_tracks.push_back(static_cast<int>(i));
            if (static_cast<int>(i) == audio_stream_index) {
                current_audio_track_index = static_cast<int>(audio_tracks.size() - 1);
            }
        }
    }

    if (audio_stream_index >= 0) {
        AVStream* audio_stream = fmt_ctx->streams[audio_stream_index];
        audio_codec_ctx = avcodec_alloc_context3(a_decoder);
        avcodec_parameters_to_context(audio_codec_ctx, audio_stream->codecpar);
        avcodec_open2(audio_codec_ctx, a_decoder, NULL);

        if (audio_engine.init_audio(audio_codec_ctx)) {
            stats.has_audio = true;
        }
    }

    reset_video_clock(0.0);

    std::cout << "[VMP Engine] File Info:" << std::endl;
    std::cout << " -> Resolution: " << stats.width << "x" << stats.height << std::endl;
    std::cout << " -> Duration: " << static_cast<int>(duration_sec) << " seconds" << std::endl;
    std::cout << " -> Native FPS: " << stream_fps << std::endl;
    std::cout << " -> Video Codec: " << stats.codec_name << std::endl;
    std::cout << " -> Audio Stream: " << (stats.has_audio ? "ACTIVE (SDL2 Low-Latency Sync)" : "NONE") << std::endl;
    std::cout << " -> Acceleration Mode: " << stats.hw_acceleration_status << std::endl;

    return true;
}

void VideoPlayer::reset_video_clock(double base_pts) {
    video_clock_start = std::chrono::high_resolution_clock::now();
    video_clock_base_pts = base_pts;
    video_clock_initialized = true;
}

void VideoPlayer::set_playback_speed(float speed) { 
    playback_speed = std::max(0.25f, std::min(4.0f, speed)); 
    audio_engine.set_playback_speed(playback_speed);
    reset_video_clock(current_pts_sec.load());
}

void VideoPlayer::toggle_pause() {
    paused = !paused;
    audio_engine.set_paused(paused.load());
    if (!paused) {
        reset_video_clock(current_pts_sec.load());
    }
    std::cout << "\n[VMP Engine] Playback " << (paused ? "PAUSED ❚❚" : "RESUMED ▶") << std::endl;
}

void VideoPlayer::set_paused(bool pause_state) {
    paused = pause_state;
    audio_engine.set_paused(pause_state);
    if (!paused) {
        reset_video_clock(current_pts_sec.load());
    }
}

void VideoPlayer::seek_to_time(double seconds) {
    if (!fmt_ctx || video_stream_index < 0) return;
    if (duration_sec > 0.0) {
        seconds = std::max(0.0, std::min(duration_sec, seconds));
    } else {
        seconds = std::max(0.0, seconds);
    }
    seek_target_sec = seconds;
    preroll_target_sec = seconds;
    seek_requested = true;
    current_pts_sec = seconds;
    reset_video_clock(seconds);
    audio_engine.set_audio_clock(seconds);
    std::cout << "\n[VMP Engine] Seek to: " << static_cast<int>(seconds) << "s" << std::endl;
}

void VideoPlayer::seek_to_ratio(double ratio) {
    ratio = std::max(0.0, std::min(1.0, ratio));
    if (duration_sec > 0.0) {
        seek_to_time(ratio * duration_sec);
    }
}

void VideoPlayer::seek_relative(double seconds) {
    seek_to_time(current_pts_sec.load() + seconds);
}

void VideoPlayer::start() {
    if (playing) return;
    playing = true;
    audio_engine.set_paused(paused.load());
    last_render_fps_check = std::chrono::high_resolution_clock::now();
    rendered_frames_count = 0;
    rendered_fps = stream_fps;
    decode_thread = std::thread(&VideoPlayer::decode_loop, this);
}

void VideoPlayer::stop() {
    playing = false;
    audio_engine.set_paused(true);
    if (decode_thread.joinable()) {
        decode_thread.join();
    }
    flush_frame_queue();
}

void VideoPlayer::decode_loop() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* raw_frame = av_frame_alloc();

    AVRational video_tb = {1, 1000};
    AVRational audio_tb = {1, 1000};

    if (fmt_ctx && video_stream_index >= 0) {
        video_tb = fmt_ctx->streams[video_stream_index]->time_base;
    }

    if (fmt_ctx && audio_stream_index >= 0) {
        audio_tb = fmt_ctx->streams[audio_stream_index]->time_base;
    }

    while (playing) {
        if (seek_requested.exchange(false)) {
            double target_sec = seek_target_sec.load();
            preroll_target_sec = target_sec;
            seeking_preroll = true;

            if (fmt_ctx) {
                int64_t target_ts = static_cast<int64_t>(target_sec * AV_TIME_BASE);
                int ret_seek = avformat_seek_file(fmt_ctx, -1, INT64_MIN, target_ts, target_ts, 0);
                if (ret_seek < 0 && video_stream_index >= 0) {
                    AVStream* st = fmt_ctx->streams[video_stream_index];
                    int64_t v_target_pts = av_rescale_q(target_ts, AV_TIME_BASE_Q, st->time_base);
                    av_seek_frame(fmt_ctx, video_stream_index, v_target_pts, AVSEEK_FLAG_BACKWARD);
                }

                if (video_codec_ctx) avcodec_flush_buffers(video_codec_ctx);
                if (audio_codec_ctx) avcodec_flush_buffers(audio_codec_ctx);

                {
                    std::lock_guard<std::mutex> lock(frame_mutex);
                    while (!frame_queue.empty()) {
                        AVFrame* f = frame_queue.front().frame;
                        if (f) av_frame_free(&f);
                        frame_queue.pop();
                    }
                }

                audio_engine.flush();
                audio_engine.set_audio_clock(target_sec);
                reset_video_clock(target_sec);
            }
        }

        if (paused && !seeking_preroll.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Flow control: throttle if video queue is full AND audio buffer is adequate
        size_t current_q_size = 0;
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            current_q_size = frame_queue.size();
            stats.buffered_frames = static_cast<int>(current_q_size);
        }

        size_t max_q = get_max_queue_size();
        if (current_q_size >= max_q && !seeking_preroll.load()) {
            if (!stats.has_audio || audio_engine.get_buffer_size() >= 32768) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
        }

        int ret = av_read_frame(fmt_ctx, packet);
        if (ret < 0) {
            if (seeking_preroll.load()) {
                seeking_preroll = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Process Video Stream
        if (packet->stream_index == video_stream_index && video_codec_ctx) {
            auto start_time = std::chrono::high_resolution_clock::now();

            auto process_video_frame_fn = [&](AVFrame* in_frame) {
                AVFrame* src_frame = in_frame;

                // 1. Hardware decoding transfer if frame is in GPU surface format (VAAPI / CUDA / VDPAU / DRM)
                AVPixelFormat hw_best_fmt = hw_decoder.find_best_pixel_format(video_codec_ctx, hw_decoder.active_device_type);
                if (in_frame->format == hw_best_fmt ||
                    in_frame->format == AV_PIX_FMT_VAAPI ||
                    in_frame->format == AV_PIX_FMT_CUDA ||
                    in_frame->format == AV_PIX_FMT_VDPAU ||
                    in_frame->format == AV_PIX_FMT_DRM_PRIME ||
                    in_frame->hw_frames_ctx != nullptr) {

                    av_frame_unref(sw_frame);
                    if (av_hwframe_transfer_data(sw_frame, in_frame, 0) == 0) {
                        sw_frame->pts = in_frame->pts;
                        sw_frame->width = in_frame->width;
                        sw_frame->height = in_frame->height;
                        src_frame = sw_frame;
                    } else {
                        // HW transfer failed; avoid passing invalid GPU surface to software stages
                        return;
                    }
                }

                // 2. Direct GPU shader rendering for NV12, P010LE, YUV420P, YUVJ420P, YUV420P10LE, RGB24, RGBA
                if (src_frame->format != AV_PIX_FMT_NV12 &&
                    src_frame->format != AV_PIX_FMT_P010LE &&
                    src_frame->format != AV_PIX_FMT_P010BE &&
                    src_frame->format != AV_PIX_FMT_YUV420P &&
                    src_frame->format != AV_PIX_FMT_YUVJ420P &&
                    src_frame->format != AV_PIX_FMT_YUV420P10LE &&
                    src_frame->format != AV_PIX_FMT_YUV420P10BE &&
                    src_frame->format != AV_PIX_FMT_RGB24 &&
                    src_frame->format != AV_PIX_FMT_RGBA) {

                    sws_ctx = sws_getCachedContext(
                        sws_ctx,
                        src_frame->width, src_frame->height, (AVPixelFormat)src_frame->format,
                        src_frame->width, src_frame->height, AV_PIX_FMT_YUV420P,
                        SWS_BILINEAR, NULL, NULL, NULL
                    );

                    if (sws_ctx) {
                        av_frame_unref(converted_frame);
                        converted_frame->width = src_frame->width;
                        converted_frame->height = src_frame->height;
                        converted_frame->format = AV_PIX_FMT_YUV420P;
                        if (av_frame_get_buffer(converted_frame, 32) == 0) {
                            sws_scale(
                                sws_ctx,
                                src_frame->data, src_frame->linesize,
                                0, src_frame->height,
                                converted_frame->data, converted_frame->linesize
                            );
                            converted_frame->pts = src_frame->pts;
                            src_frame = converted_frame;
                        }
                    }
                }

                auto end_time = std::chrono::high_resolution_clock::now();
                double duration = std::chrono::duration<double, std::milli>(end_time - start_time).count();

                double v_pts_sec = 0.0;
                if (src_frame->pts != AV_NOPTS_VALUE && video_stream_index >= 0) {
                    v_pts_sec = src_frame->pts * av_q2d(video_tb);
                }

                double frame_dur = (stream_fps > 0.0) ? (1.0 / stream_fps) : 0.04166;
                double p_target = preroll_target_sec.load();

                if (seeking_preroll.load()) {
                    if (v_pts_sec < p_target - (frame_dur * 0.5)) {
                        // Preroll reference frame (needed for decoding subsequent frames, but skip display)
                        return;
                    } else {
                        // Target frame reached!
                        seeking_preroll = false;
                        current_pts_sec = v_pts_sec;
                        reset_video_clock(v_pts_sec);
                    }
                }

                if (paused) {
                    std::lock_guard<std::mutex> lock(frame_mutex);
                    av_frame_unref(current_frame);
                    av_frame_ref(current_frame, src_frame);
                    current_pts_sec = v_pts_sec;
                    has_new_frame = true;
                    seeking_preroll = false;
                } else {
                    // Enqueue decoded frame into multi-frame buffer
                    AVFrame* q_frame = av_frame_alloc();
                    if (q_frame) {
                        av_frame_ref(q_frame, src_frame);
                        QueuedVideoFrame q_item;
                        q_item.frame = q_frame;
                        q_item.pts = v_pts_sec;
                        q_item.decode_time_ms = duration;

                        {
                            std::lock_guard<std::mutex> lock(frame_mutex);
                            frame_queue.push(q_item);
                            stats.decode_time_ms = duration;
                            stats.total_frames_decoded++;
                        }
                    }
                }
            };

            int send_ret = avcodec_send_packet(video_codec_ctx, packet);
            while (send_ret == AVERROR(EAGAIN) && playing) {
                // Internal buffers full: drain frames until packet can be accepted
                int r = avcodec_receive_frame(video_codec_ctx, raw_frame);
                if (r >= 0) {
                    process_video_frame_fn(raw_frame);
                } else if (r == AVERROR(EAGAIN)) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                } else {
                    break;
                }
                send_ret = avcodec_send_packet(video_codec_ctx, packet);
            }

            // Drain any frames produced after packet was accepted
            while (send_ret >= 0 && playing) {
                int r = avcodec_receive_frame(video_codec_ctx, raw_frame);
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF || r < 0) {
                    break;
                }
                process_video_frame_fn(raw_frame);
            }
        } 
        // Process Audio Stream (Thread-safe without frame_mutex lock)
        else if (packet->stream_index == audio_stream_index && audio_codec_ctx) {
            auto process_audio_frame_fn = [&](AVFrame* in_audio_frame) {
                double audio_pts = (in_audio_frame->pts != AV_NOPTS_VALUE) ? in_audio_frame->pts * av_q2d(audio_tb) : -1.0;
                double audio_dur = (audio_codec_ctx->sample_rate > 0) ? (static_cast<double>(in_audio_frame->nb_samples) / audio_codec_ctx->sample_rate) : 0.0;
                double p_target = preroll_target_sec.load();

                if (seeking_preroll.load()) {
                    if (audio_pts >= 0.0 && audio_pts + audio_dur < p_target) {
                        return; // Drop audio frame prior to seek target
                    }
                }

                if (!paused) {
                    audio_engine.play_chunk(in_audio_frame, audio_pts);
                }
            };

            int ret_a = avcodec_send_packet(audio_codec_ctx, packet);
            while (ret_a == AVERROR(EAGAIN) && playing) {
                int r = avcodec_receive_frame(audio_codec_ctx, audio_frame);
                if (r >= 0) {
                    process_audio_frame_fn(audio_frame);
                } else if (r == AVERROR(EAGAIN)) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                } else {
                    break;
                }
                ret_a = avcodec_send_packet(audio_codec_ctx, packet);
            }

            while (ret_a >= 0 && playing) {
                int r = avcodec_receive_frame(audio_codec_ctx, audio_frame);
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF || r < 0) break;
                process_audio_frame_fn(audio_frame);
            }
        }

        av_packet_unref(packet);
    }

    av_frame_free(&raw_frame);
    av_packet_free(&packet);
}

void VideoPlayer::render_current_frame(ShaderRenderer& renderer, int win_w, int win_h, bool menu_bar_visible) {
    bool upload_needed = false;
    AVPixelFormat upload_format = AV_PIX_FMT_NONE;

    {
        std::lock_guard<std::mutex> lock(frame_mutex);
        if (!paused && !frame_queue.empty()) {
            double sync_clock = 0.0;
            bool has_valid_audio = false;
            if (stats.has_audio) {
                sync_clock = audio_engine.get_audio_clock();
                if (sync_clock > 0.0 || (duration_sec > 0.0 && sync_clock <= duration_sec)) {
                    has_valid_audio = true;
                }
            }

            if (!has_valid_audio) {
                // High-precision Monotonic Video Clock Pacing for silent / video-only files
                auto now_t = std::chrono::high_resolution_clock::now();
                if (!video_clock_initialized) {
                    video_clock_start = now_t;
                    video_clock_base_pts = frame_queue.front().pts;
                    video_clock_initialized = true;
                }

                double elapsed_sec = std::chrono::duration<double>(now_t - video_clock_start).count() * playback_speed;
                sync_clock = video_clock_base_pts + elapsed_sec;
            }

            double frame_duration = (stream_fps > 0.0) ? (1.0 / stream_fps) : 0.01666;
            double early_tolerance = std::min(0.012, frame_duration * 0.45);
            double late_threshold = std::max(0.04, frame_duration * 2.0);

            // If the front frame has already been superseded by the next frame (e.g. 120 FPS on 60Hz display,
            // or after any momentary hitch), discard the stale frame immediately to maintain lockstep with audio
            while (frame_queue.size() > 1 && frame_queue.front().pts < sync_clock - early_tolerance) {
                QueuedVideoFrame stale = frame_queue.front();
                frame_queue.pop();
                if (stale.frame) av_frame_free(&stale.frame);
            }

            // Also prune severely late frames
            while (frame_queue.size() > 1 && frame_queue.front().pts < sync_clock - late_threshold) {
                QueuedVideoFrame stale = frame_queue.front();
                frame_queue.pop();
                if (stale.frame) av_frame_free(&stale.frame);
            }

            if (!frame_queue.empty() && frame_queue.front().pts <= sync_clock + early_tolerance) {
                QueuedVideoFrame q_item = frame_queue.front();
                frame_queue.pop();

                av_frame_unref(current_frame);
                av_frame_move_ref(current_frame, q_item.frame);
                av_frame_free(&q_item.frame);

                current_pts_sec = q_item.pts;
                has_new_frame = true;
                rendered_frames_count++;
            }
        }

        // Measure actual rendered display FPS smoothly
        auto now = std::chrono::high_resolution_clock::now();
        double render_elapsed = std::chrono::duration<double>(now - last_render_fps_check).count();
        if (render_elapsed >= 0.5) {
            rendered_fps = static_cast<double>(rendered_frames_count) / render_elapsed;
            rendered_frames_count = 0;
            last_render_fps_check = now;
            stats.fps = (rendered_fps > 0.1) ? rendered_fps : stream_fps;
        }

        if (has_new_frame && current_frame && current_frame->data[0]) {
            upload_needed = true;
            upload_format = static_cast<AVPixelFormat>(current_frame->format);
            has_new_frame = false;
        }
    }

    // Upload textures OUTSIDE the frame_mutex lock so decoding thread is never stalled by GPU texture uploads!
    if (upload_needed && current_frame && current_frame->data[0]) {
        if (upload_format == AV_PIX_FMT_NV12) {
            renderer.upload_nv12_frame(
                current_frame->data[0], current_frame->data[1],
                current_frame->linesize[0], current_frame->linesize[1],
                current_frame->width, current_frame->height
            );
        } else if (upload_format == AV_PIX_FMT_P010LE || upload_format == AV_PIX_FMT_P010BE) {
            renderer.upload_p010_frame(
                current_frame->data[0], current_frame->data[1],
                current_frame->linesize[0], current_frame->linesize[1],
                current_frame->width, current_frame->height
            );
        } else if (upload_format == AV_PIX_FMT_YUV420P || upload_format == AV_PIX_FMT_YUVJ420P) {
            renderer.upload_yuv_frame(
                current_frame->data[0], current_frame->data[1], current_frame->data[2],
                current_frame->linesize[0], current_frame->linesize[1], current_frame->linesize[2],
                current_frame->width, current_frame->height
            );
        } else if (upload_format == AV_PIX_FMT_YUV420P10LE || upload_format == AV_PIX_FMT_YUV420P10BE) {
            renderer.upload_yuv10_frame(
                current_frame->data[0], current_frame->data[1], current_frame->data[2],
                current_frame->linesize[0], current_frame->linesize[1], current_frame->linesize[2],
                current_frame->width, current_frame->height
            );
        } else if (upload_format == AV_PIX_FMT_RGB24 || upload_format == AV_PIX_FMT_RGBA) {
            renderer.upload_rgb_frame(
                current_frame->data[0],
                current_frame->width, current_frame->height
            );
        }
    }

    renderer.render(win_w, win_h, menu_bar_visible);
}

VMPStats VideoPlayer::get_stats() {
    std::lock_guard<std::mutex> lock(frame_mutex);
    stats.buffered_frames = static_cast<int>(frame_queue.size());
    stats.stream_fps = stream_fps;
    return stats;
}

std::vector<std::string> VideoPlayer::get_audio_track_list() {
    std::lock_guard<std::mutex> lock(frame_mutex);
    std::vector<std::string> track_list;
    if (!fmt_ctx) return track_list;

    int idx = 1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            std::string lang = "unknown";
            AVDictionaryEntry* lang_tag = av_dict_get(fmt_ctx->streams[i]->metadata, "language", NULL, 0);
            if (lang_tag) {
                lang = lang_tag->value;
            }
            AVDictionaryEntry* title_tag = av_dict_get(fmt_ctx->streams[i]->metadata, "title", NULL, 0);
            std::string title = "";
            if (title_tag) {
                title = title_tag->value;
            }
            const char* codec_name_ptr = avcodec_get_name(fmt_ctx->streams[i]->codecpar->codec_id);
            std::string codec = codec_name_ptr ? codec_name_ptr : "unknown";
            
            std::string desc = "Track " + std::to_string(idx) + " [" + lang + "]";
            if (!title.empty()) {
                desc += " - " + title;
            }
            desc += " (" + codec + ")";
            track_list.push_back(desc);
            idx++;
        }
    }
    return track_list;
}

bool VideoPlayer::select_audio_track(int track_vector_idx) {
    std::lock_guard<std::mutex> lock(frame_mutex);
    if (!fmt_ctx || track_vector_idx < 0 || track_vector_idx >= static_cast<int>(audio_tracks.size())) {
        return false;
    }

    int new_stream_idx = audio_tracks[track_vector_idx];
    if (new_stream_idx == audio_stream_index) {
        return true; // Already selected
    }

    std::cout << "[VMP Engine] Switching audio track to stream index: " << new_stream_idx << std::endl;

    // 1. Close current codec context
    if (audio_codec_ctx) {
        avcodec_free_context(&audio_codec_ctx);
        audio_codec_ctx = nullptr;
    }

    // 2. Open new codec context
    audio_stream_index = new_stream_idx;
    current_audio_track_index = track_vector_idx;
    
    AVStream* audio_stream = fmt_ctx->streams[audio_stream_index];
    const AVCodec* a_decoder = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    if (!a_decoder) {
        std::cerr << "[VMP Error] Decoder not found for new audio stream." << std::endl;
        stats.has_audio = false;
        return false;
    }

    audio_codec_ctx = avcodec_alloc_context3(a_decoder);
    if (avcodec_parameters_to_context(audio_codec_ctx, audio_stream->codecpar) < 0) {
        std::cerr << "[VMP Error] Failed to copy codec parameters to context." << std::endl;
        avcodec_free_context(&audio_codec_ctx);
        audio_codec_ctx = nullptr;
        stats.has_audio = false;
        return false;
    }

    if (avcodec_open2(audio_codec_ctx, a_decoder, NULL) < 0) {
        std::cerr << "[VMP Error] Failed to open new audio decoder." << std::endl;
        avcodec_free_context(&audio_codec_ctx);
        audio_codec_ctx = nullptr;
        stats.has_audio = false;
        return false;
    }

    // 3. Re-initialize and flush audio engine
    audio_engine.close(); // close previous device/buffer
    if (audio_engine.init_audio(audio_codec_ctx)) {
        stats.has_audio = true;
        std::cout << "[VMP Engine] Audio track switched successfully." << std::endl;
        return true;
    } else {
        std::cerr << "[VMP Error] Failed to re-initialize audio engine for new track." << std::endl;
        stats.has_audio = false;
        return false;
    }
}
