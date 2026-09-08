#include <iostream>
#include <iomanip>
#include <GLFW/glfw3.h>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <unordered_set>
#include <algorithm>

#include "video_player.h"
#include "shader_renderer.h"
#include "telemetry.h"
#include "subtitles.h"
#include "resume_manager.h"
#include "thumbnail_generator.h"
#include "material_icons_data.h"
#include "app_icon_data.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

static bool extract_media_metadata_and_thumb(const std::string& filepath, std::string& out_duration_str, double& out_duration_sec, std::vector<uint8_t>& out_thumb_rgb, int& out_tw, int& out_th) {
    out_tw = 160;
    out_th = 90;
    out_duration_sec = 0.0;
    out_duration_str = "00:00";

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filepath.c_str(), NULL, NULL) < 0) {
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    if (fmt_ctx->duration > 0) {
        out_duration_sec = static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE;
    }

    const AVCodec* decoder = nullptr;
    int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    
    if (video_stream_idx >= 0 && decoder) {
        AVStream* st = fmt_ctx->streams[video_stream_idx];
        if (out_duration_sec <= 0.0 && st->duration > 0) {
            out_duration_sec = st->duration * av_q2d(st->time_base);
        }

        int total_s = static_cast<int>(std::max(0.0, out_duration_sec));
        int hrs = total_s / 3600;
        int mins = (total_s % 3600) / 60;
        int secs = total_s % 60;
        char dbuf[32];
        if (hrs > 0) snprintf(dbuf, sizeof(dbuf), "%02d:%02d:%02d", hrs, mins, secs);
        else snprintf(dbuf, sizeof(dbuf), "%02d:%02d", mins, secs);
        out_duration_str = dbuf;

        AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
        if (codec_ctx) {
            avcodec_parameters_to_context(codec_ctx, st->codecpar);
            codec_ctx->thread_count = 1;
            if (avcodec_open2(codec_ctx, decoder, NULL) >= 0) {
                double target_sec = std::min(5.0, out_duration_sec * 0.1);
                int64_t target_pts = av_rescale_q(static_cast<int64_t>(target_sec * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
                av_seek_frame(fmt_ctx, video_stream_idx, target_pts, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(codec_ctx);

                AVFrame* frame = av_frame_alloc();
                AVFrame* rgb_frame = av_frame_alloc();
                AVPacket* packet = av_packet_alloc();

                rgb_frame->format = AV_PIX_FMT_RGB24;
                rgb_frame->width = out_tw;
                rgb_frame->height = out_th;
                av_frame_get_buffer(rgb_frame, 32);

                SwsContext* sws_ctx = sws_getContext(
                    codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                    out_tw, out_th, AV_PIX_FMT_RGB24,
                    SWS_BILINEAR, NULL, NULL, NULL
                );

                bool decoded = false;
                int attempts = 0;
                while (!decoded && attempts < 25 && av_read_frame(fmt_ctx, packet) >= 0) {
                    attempts++;
                    if (packet->stream_index == video_stream_idx) {
                        if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                            if (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                                if (sws_ctx) {
                                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, codec_ctx->height,
                                              rgb_frame->data, rgb_frame->linesize);
                                    out_thumb_rgb.resize(out_tw * out_th * 3);
                                    for (int y = 0; y < out_th; y++) {
                                        memcpy(out_thumb_rgb.data() + y * out_tw * 3,
                                               rgb_frame->data[0] + y * rgb_frame->linesize[0],
                                               out_tw * 3);
                                    }
                                    decoded = true;
                                }
                            }
                        }
                    }
                    av_packet_unref(packet);
                }

                if (sws_ctx) sws_freeContext(sws_ctx);
                av_frame_free(&frame);
                av_frame_free(&rgb_frame);
                av_packet_free(&packet);
            }
            avcodec_free_context(&codec_ctx);
        }
    } else {
        int total_s = static_cast<int>(std::max(0.0, out_duration_sec));
        int hrs = total_s / 3600;
        int mins = (total_s % 3600) / 60;
        int secs = total_s % 60;
        char dbuf[32];
        if (hrs > 0) snprintf(dbuf, sizeof(dbuf), "%02d:%02d:%02d", hrs, mins, secs);
        else snprintf(dbuf, sizeof(dbuf), "%02d:%02d", mins, secs);
        out_duration_str = dbuf;
    }

    avformat_close_input(&fmt_ctx);
    return true;
}

static std::string get_file_size_string(const std::string& path) {
    try {
        if (!std::filesystem::exists(path)) return "N/A";
        auto size = std::filesystem::file_size(path);
        double size_mb = static_cast<double>(size) / (1024.0 * 1024.0);
        char buf[64];
        if (size_mb >= 1024.0) {
            double size_gb = size_mb / 1024.0;
            snprintf(buf, sizeof(buf), "%.2f GB", size_gb);
        } else if (size_mb < 0.1) {
            double size_kb = static_cast<double>(size) / 1024.0;
            snprintf(buf, sizeof(buf), "%.2f KB", size_kb);
        } else {
            snprintf(buf, sizeof(buf), "%.2f MB", size_mb);
        }
        return std::string(buf);
    } catch (...) {
        return "N/A";
    }
}

static std::string get_file_date_string(const std::string& path) {
    try {
        if (!std::filesystem::exists(path)) return "N/A";
        auto ftime = std::filesystem::last_write_time(path);
        auto sct = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
        std::time_t ctime = std::chrono::system_clock::to_time_t(sct);
        std::tm* tm = std::localtime(&ctime);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
        return std::string(buf);
    } catch (...) {
        return "N/A";
    }
}

static std::string format_time(double seconds, bool force_hours = false) {
    int total_sec = static_cast<int>(std::max(0.0, seconds));
    int hrs = total_sec / 3600;
    int mins = (total_sec % 3600) / 60;
    int secs = total_sec % 60;
    char buf[64];
    if (hrs > 0 || force_hours) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hrs, mins, secs);
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    }
    return std::string(buf);
}

static bool is_image_file(const std::string& path) {
    std::string ext = "";
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        for (char& c : ext) c = std::tolower(c);
    }
    static const std::unordered_set<std::string> img_exts = {
        ".jpg", ".jpeg", ".png", ".bmp", ".svg", ".webp", ".gif", ".tiff", ".tif", ".ico", ".jfif", ".avif", ".heic"
    };
    return img_exts.count(ext) > 0;
}

static bool is_video_file(const std::string& path) {
    std::string ext = "";
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        for (char& c : ext) c = std::tolower(c);
    }
    static const std::unordered_set<std::string> vid_exts = {
        ".mp4", ".mkv", ".webm", ".avi", ".mov", ".flv", ".ts", ".m4v", ".wmv", ".3gp", ".ogv", ".m2ts"
    };
    return vid_exts.count(ext) > 0;
}

static double g_scroll_y = 0.0;
static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)window;
    (void)xoffset;
    g_scroll_y += yoffset;
}

static std::vector<std::string> g_dropped_files;
static std::mutex g_dropped_mutex;
static void drop_callback(GLFWwindow* window, int count, const char** paths) {
    (void)window;
    std::lock_guard<std::mutex> lock(g_dropped_mutex);
    for (int i = 0; i < count; i++) {
        if (paths[i]) g_dropped_files.push_back(paths[i]);
    }
}

static void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    (void)window;
    if (width > 0 && height > 0) {
        glViewport(0, 0, width, height);
    }
}

int main(int argc, char** argv) {
    std::cout << "============================================================" << std::endl;
    std::cout << "      VMP ENGINE v0.0.2-beta - Video Max Player Platform    " << std::endl;
    std::cout << "  High-Performance | Audio Sync | HDR Shaders | Telemetry   " << std::endl;
    std::cout << "============================================================" << std::endl;

    std::vector<std::string> playlist_files;
    std::string export_filepath = "";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--export-stats") {
            if (i + 1 < argc) {
                export_filepath = argv[i + 1];
                i++;
            }
        } else if (arg == "--hw-accel") {
            if (i + 1 < argc) {
                std::string mode_str = argv[i + 1];
                setenv("VMP_HW_ACCEL", mode_str.c_str(), 1);
                i++;
            }
        } else if (arg == "--no-hw" || arg == "--cpu") {
            setenv("VMP_HW_ACCEL", "cpu", 1);
            setenv("VMP_FORCE_CPU", "1", 1);
        } else if (arg == "--force-hw" || arg == "--vaapi") {
            setenv("VMP_HW_ACCEL", "vaapi", 1);
        } else if (arg == "--version" || arg == "-v") {
            std::cout << "\nVMP v0.0.2-beta (Linux x86_64)" << std::endl;
            std::cout << "Ultra-Native High-Performance Video Max Player Engine" << std::endl;
            std::cout << "Version: v0.0.2-beta (Beta Preview)" << std::endl;
            std::cout << "Standard: C++20 | Optimizations: AVX2, FMA, LTO" << std::endl;
            std::cout << "Graphics: OpenGL 3.3 Core Profile | Audio: SDL2" << std::endl;
            std::cout << "Decoders: FFmpeg libavcodec + VA-API / NVDEC + AVX2 Multi-Thread" << std::endl;
            std::cout << "License: MIT" << std::endl;
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "\n[Usage]: vmp_engine [path_to_video_file] [options]" << std::endl;
            std::cout << "         vmp_engine sample.mp4 --hw-accel <auto|vaapi|cuda|cpu>" << std::endl;
            std::cout << "         vmp_engine sample.mp4 --export-stats report.json" << std::endl;
            std::cout << "         vmp_engine --version" << std::endl;
            return 0;
        } else {
            if (is_image_file(arg)) {
                std::cerr << "[VMP Error] Image files are not supported: " << arg 
                          << " (VMP is dedicated to video playback only)" << std::endl;
                return 1;
            } else {
                playlist_files.push_back(arg);
            }
        }
    }

    size_t current_file_idx = 0;
    std::string video_filename = "";
    bool show_stats = false;
    bool loop_mode = false;
    std::string video_size = "N/A";
    std::string video_date = "N/A";

    std::string srt_filepath = "";
    std::vector<SubtitleEntry> subtitle_entries;
    bool show_subtitles = true;
    std::string osd_notification = "";
    auto osd_notification_time = std::chrono::high_resolution_clock::now();

    if (!glfwInit()) {
        std::cerr << "[VMP Error] Failed to initialize GLFW!" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Set window class and instance hints so that window managers and taskbars/panels (X11 & Wayland)
    // associate the window with vmp.desktop rather than defaulting to the window title ("VMP").
    // This ensures the app icon remains consistent and does not change or revert to default when multiple windows are open and grouped.
#if defined(GLFW_X11_CLASS_NAME) && defined(GLFW_X11_INSTANCE_NAME)
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "vmp");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "vmp");
#endif
#if defined(GLFW_WAYLAND_APP_ID)
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "vmp");
#endif

    GLFWwindow* window = glfwCreateWindow(1024, 576, "VMP", NULL, NULL);
    if (!window) {
        std::cerr << "[VMP Error] Failed to create OpenGL Window!" << std::endl;
        glfwTerminate();
        return -1;
    }

    // Set Window Icon with multiple resolutions (128, 64, 48, 32, 16)
    // for crisp display across taskbars, alt-tab switchers, and window decorations
    auto downsample_rgba_box = [](const uint8_t* src, int src_w, int src_h, int dst_w, int dst_h) -> std::vector<uint8_t> {
        std::vector<uint8_t> dst(dst_w * dst_h * 4);
        for (int y = 0; y < dst_h; y++) {
            int y0 = (y * src_h) / dst_h;
            int y1 = ((y + 1) * src_h) / dst_h;
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < dst_w; x++) {
                int x0 = (x * src_w) / dst_w;
                int x1 = ((x + 1) * src_w) / dst_w;
                if (x1 <= x0) x1 = x0 + 1;
                uint32_t r = 0, g = 0, b = 0, a = 0;
                int count = 0;
                for (int sy = y0; sy < y1; sy++) {
                    for (int sx = x0; sx < x1; sx++) {
                        int idx = (sy * src_w + sx) * 4;
                        r += src[idx + 0];
                        g += src[idx + 1];
                        b += src[idx + 2];
                        a += src[idx + 3];
                        count++;
                    }
                }
                int dst_idx = (y * dst_w + x) * 4;
                dst[dst_idx + 0] = (uint8_t)(r / count);
                dst[dst_idx + 1] = (uint8_t)(g / count);
                dst[dst_idx + 2] = (uint8_t)(b / count);
                dst[dst_idx + 3] = (uint8_t)(a / count);
            }
        }
        return dst;
    };

    std::vector<uint8_t> icon_64 = downsample_rgba_box(VMP_APP_ICON_RGBA, 128, 128, 64, 64);
    std::vector<uint8_t> icon_48 = downsample_rgba_box(VMP_APP_ICON_RGBA, 128, 128, 48, 48);
    std::vector<uint8_t> icon_32 = downsample_rgba_box(VMP_APP_ICON_RGBA, 128, 128, 32, 32);
    std::vector<uint8_t> icon_16 = downsample_rgba_box(VMP_APP_ICON_RGBA, 128, 128, 16, 16);

    GLFWimage app_icons[5];
    app_icons[0] = { VMP_APP_ICON_SIZE, VMP_APP_ICON_SIZE, const_cast<unsigned char*>(VMP_APP_ICON_RGBA) };
    app_icons[1] = { 64, 64, icon_64.data() };
    app_icons[2] = { 48, 48, icon_48.data() };
    app_icons[3] = { 32, 32, icon_32.data() };
    app_icons[4] = { 16, 16, icon_16.data() };
    glfwSetWindowIcon(window, 5, app_icons);

    int windowed_x = 100, windowed_y = 100;
    int windowed_w = 1024, windowed_h = 576;

    // Center Window Immediately on Primary Monitor Workarea
    GLFWmonitor* primary_mon = glfwGetPrimaryMonitor();
    if (primary_mon) {
        int work_x = 0, work_y = 0, work_w = 1920, work_h = 1080;
        glfwGetMonitorWorkarea(primary_mon, &work_x, &work_y, &work_w, &work_h);
        windowed_x = work_x + (work_w - windowed_w) / 2;
        windowed_y = work_y + (work_h - windowed_h) / 2;
        glfwSetWindowPos(window, windowed_x, windowed_y);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Standard VSync mode for tear-free, smooth rendering and low CPU usage
    glfwSetInputMode(window, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetDropCallback(window, drop_callback);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    ShaderRenderer renderer;
    if (!renderer.init_gl_shaders()) {
        std::cerr << "[VMP Error] Failed to initialize Shader Renderer!" << std::endl;
        return -1;
    }

    VideoPlayer player;
    TelemetryExporter telemetry;

    bool is_fullscreen = false;
    int active_menu_idx = -1;
    bool context_menu_active = false;
    float context_menu_x = 0.0f;
    float context_menu_y = 0.0f;
    int context_submenu_idx = -1;
    bool mouse_right_prev = false;

    auto get_window_monitor_fn = [](GLFWwindow* win) -> GLFWmonitor* {
        int wx, wy, ww, wh;
        glfwGetWindowPos(win, &wx, &wy);
        glfwGetWindowSize(win, &ww, &wh);
        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        if (!monitors || count == 0) return glfwGetPrimaryMonitor();
        GLFWmonitor* best = monitors[0];
        int max_overlap = -1;
        for (int i = 0; i < count; i++) {
            int mx, my;
            glfwGetMonitorPos(monitors[i], &mx, &my);
            const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
            if (!mode) continue;
            int overlap_x = std::max(0, std::min(wx + ww, mx + mode->width) - std::max(wx, mx));
            int overlap_y = std::max(0, std::min(wy + wh, my + mode->height) - std::max(wy, my));
            int overlap = overlap_x * overlap_y;
            if (overlap > max_overlap) {
                max_overlap = overlap;
                best = monitors[i];
            }
        }
        return best ? best : glfwGetPrimaryMonitor();
    };

    auto toggle_fullscreen_fn = [&](GLFWwindow* win) {
        active_menu_idx = -1;
        context_menu_active = false;
        context_submenu_idx = -1;
        if (!is_fullscreen) {
            glfwGetWindowPos(win, &windowed_x, &windowed_y);
            glfwGetWindowSize(win, &windowed_w, &windowed_h);
            windowed_w = std::max(640, windowed_w);
            windowed_h = std::max(360, windowed_h);
            GLFWmonitor* monitor = get_window_monitor_fn(win);
            if (!monitor) monitor = glfwGetPrimaryMonitor();
            if (monitor) {
                const GLFWvidmode* mode = glfwGetVideoMode(monitor);
                if (mode) {
                    glfwSetWindowMonitor(win, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
                    is_fullscreen = true;
                    std::cout << "\n[VMP Engine] Fullscreen Mode: ENABLED (" << mode->width << "x" << mode->height << ")" << std::endl;
                }
            }
        } else {
            glfwSetWindowMonitor(win, NULL, windowed_x, windowed_y, windowed_w, windowed_h, 0);
            is_fullscreen = false;
            std::cout << "\n[VMP Engine] Fullscreen Mode: DISABLED (Windowed)" << std::endl;
        }
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(win, &fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0) {
            glViewport(0, 0, fb_w, fb_h);
        }
    };

    auto center_and_scale_window_fn = [&](int video_w, int video_h) {
        if (is_fullscreen || glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) return; // Maintain fullscreen or maximized state if user has it active

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        if (!monitor) return;

        int work_x = 0, work_y = 0, work_w = 1920, work_h = 1080;
        glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_w, &work_h);

        if (video_w <= 0 || video_h <= 0) {
            video_w = 1280;
            video_h = 720;
        }

        float menu_bar_h = 26.0f;
        // Proportional comfortable medium bounds: ~70% to 75% max of monitor workarea (accounting for menu bar)
        float max_target_w = static_cast<float>(work_w) * 0.75f;
        float max_target_h = std::max(200.0f, static_cast<float>(work_h) * 0.75f - menu_bar_h);
        float min_target_w = std::min(640.0f, static_cast<float>(work_w) * 0.5f);
        float min_target_h = std::min(360.0f, static_cast<float>(work_h) * 0.5f);

        float target_w = static_cast<float>(video_w);
        float target_h = static_cast<float>(video_h);

        if (target_w > max_target_w || target_h > max_target_h) {
            float scale = std::min(max_target_w / target_w, max_target_h / target_h);
            target_w *= scale;
            target_h *= scale;
        } else if (target_w < min_target_w || target_h < min_target_h) {
            float scale = std::max(min_target_w / target_w, min_target_h / target_h);
            if (target_w * scale <= max_target_w && target_h * scale <= max_target_h) {
                target_w *= scale;
                target_h *= scale;
            }
        }

        int final_video_w = std::max(320, static_cast<int>(std::round(target_w)));
        int final_video_h = std::max(180, static_cast<int>(std::round(target_h)));

        // Window height includes the 26px menu bar so the video retains its exact, full resolution and aspect ratio!
        int final_w = final_video_w;
        int final_h = final_video_h + static_cast<int>(menu_bar_h);

        int pos_x = work_x + (work_w - final_w) / 2;
        int pos_y = work_y + (work_h - final_h) / 2;

        windowed_x = pos_x;
        windowed_y = pos_y;
        windowed_w = final_w;
        windowed_h = final_h;

        glfwSetWindowSize(window, final_w, final_h);
        glfwSetWindowPos(window, pos_x, pos_y);

        std::cout << "[VMP Engine] Window Centered & Scaled: Video Area " 
                  << final_video_w << "x" << final_video_h << " (Window " << final_w << "x" << final_h << " with Menu Bar) at (" 
                  << pos_x << ", " << pos_y << ")" << std::endl;
    };

    ThumbnailGenerator thumb_gen;
    std::vector<SubtitleTrack> available_sub_tracks;
    int current_sub_track_idx = -1;
    auto last_resume_save = std::chrono::high_resolution_clock::now();

    // Lambda to load a file from playlist
    auto load_playlist_file_fn = [&](size_t idx) -> bool {
        if (idx >= playlist_files.size()) return false;
        
        // Save current video position before switching
        if (!video_filename.empty() && current_file_idx < playlist_files.size()) {
            ResumeManager::get_instance().save_position(playlist_files[current_file_idx], player.get_current_time(), player.get_duration());
        }
        current_file_idx = idx;

        std::string current_filepath = playlist_files[idx];
        std::string new_video_filename = current_filepath;
        size_t last_slash = current_filepath.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            new_video_filename = current_filepath.substr(last_slash + 1);
        }
        
        player.close_file();
        renderer.clear_video_frame();
        thumb_gen.stop();
        
        if (!player.open_file(current_filepath)) {
            std::cerr << "[VMP Error] Could not load file: " << current_filepath << std::endl;
            return false;
        }
        
        video_filename = new_video_filename;
        video_size = get_file_size_string(current_filepath);
        video_date = get_file_date_string(current_filepath);
        
        // Start low-overhead background thumbnail generator
        thumb_gen.start_generation(current_filepath, player.get_duration());

        // Find and load all available subtitles (ASS, SSA, SRT)
        available_sub_tracks = SubtitleParser::find_subtitle_tracks_for_video(current_filepath);
        subtitle_entries.clear();
        current_sub_track_idx = -1;
        if (!available_sub_tracks.empty()) {
            current_sub_track_idx = 0;
            subtitle_entries = available_sub_tracks[0].entries;
            std::cout << "[VMP Subtitles] Auto-loaded track: " << available_sub_tracks[0].title 
                      << " (" << subtitle_entries.size() << " entries)" << std::endl;
        }
        
        // Auto-scale and center window in middle of screen
        int video_w = player.get_stats().width;
        int video_h = player.get_stats().height;
        center_and_scale_window_fn(video_w, video_h);
        
        player.start();
        
        // Check for saved resume position
        double saved_pos = ResumeManager::get_instance().get_saved_position(current_filepath);
        if (saved_pos > 8.0) {
            player.seek_to_time(saved_pos);
            bool has_hours = (player.get_duration() >= 3600.0);
            osd_notification = "Resumed: " + format_time(saved_pos, has_hours);
            std::cout << "\n[VMP Resume] Auto-resumed playback from: " << format_time(saved_pos, has_hours) << std::endl;
        } else {
            osd_notification = "Playing: " + video_filename;
        }
        osd_notification_time = std::chrono::high_resolution_clock::now();
        
        return true;
    };

    auto open_folder_dialog_fn = [&]() -> std::string {
        std::string cmd = "zenity --file-selection --directory --title=\"Select Folder with Videos - VMP Player\" 2>/dev/null";
        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) return "";
        char buf[1024];
        std::string res = "";
        if (fgets(buf, sizeof(buf), fp)) {
            res = buf;
            while (!res.empty() && (res.back() == '\n' || res.back() == '\r')) {
                res.pop_back();
            }
        }
        pclose(fp);
        return res;
    };

    auto open_file_dialog_fn = [&]() -> std::string {
        std::string cmd = "zenity --file-selection --title=\"Select Video File - VMP Player\" --file-filter=\"Video Files | *.mp4 *.mkv *.webm *.avi *.mov *.flv *.ts *.wmv\" 2>/dev/null";
        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) return "";
        char buf[1024];
        std::string res = "";
        if (fgets(buf, sizeof(buf), fp)) {
            res = buf;
            while (!res.empty() && (res.back() == '\n' || res.back() == '\r')) {
                res.pop_back();
            }
        }
        pclose(fp);
        return res;
    };

    enum class AppState {
        WELCOME,
        FOLDER_GALLERY,
        PLAYING
    };
    AppState app_state = AppState::WELCOME;
    std::string current_folder_path = "";
    std::vector<FolderMediaItem> folder_media_items;
    int gallery_scroll_offset = 0;

    auto scan_folder_fn = [&](const std::string& dir_path) -> bool {
        current_folder_path = dir_path;
        for (auto& item : folder_media_items) {
            if (item.thumb_tex != 0) {
                glDeleteTextures(1, &item.thumb_tex);
                item.thumb_tex = 0;
            }
        }
        folder_media_items.clear();
        playlist_files.clear();
        try {
            if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
                return false;
            }
            for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
                if (entry.is_regular_file()) {
                    std::string fpath = entry.path().string();
                    if (is_video_file(fpath)) {
                        FolderMediaItem item;
                        item.filepath = fpath;
                        item.filename = entry.path().filename().string();
                        item.size_str = get_file_size_string(item.filepath);
                        item.date_str = get_file_date_string(item.filepath);

                        std::vector<uint8_t> thumb_rgb;
                        int tw = 0, th = 0;
                        extract_media_metadata_and_thumb(item.filepath, item.duration_str, item.duration_sec, thumb_rgb, tw, th);
                        if (!thumb_rgb.empty()) {
                            item.thumb_tex = ShaderRenderer::create_rgb_texture(thumb_rgb.data(), tw, th);
                            item.thumb_w = tw;
                            item.thumb_h = th;
                        }
                        folder_media_items.push_back(item);
                    }
                }
            }
            std::sort(folder_media_items.begin(), folder_media_items.end(), [](const FolderMediaItem& a, const FolderMediaItem& b) {
                return a.filename < b.filename;
            });
            for (const auto& item : folder_media_items) {
                playlist_files.push_back(item.filepath);
            }
            return !folder_media_items.empty();
        } catch (...) {
            return false;
        }
    };

    if (!playlist_files.empty()) {
        std::string first_arg = playlist_files[0];
        if (std::filesystem::is_directory(first_arg)) {
            if (scan_folder_fn(first_arg)) {
                app_state = AppState::FOLDER_GALLERY;
            } else {
                std::cerr << "[VMP Error] No video files found in folder: " << first_arg << std::endl;
                return 1;
            }
        } else {
            if (load_playlist_file_fn(0)) {
                app_state = AppState::PLAYING;
            } else {
                std::cerr << "[VMP Error] Failed to open video file: " << first_arg << std::endl;
                return 1;
            }
        }
    }

    float current_zoom = 1.0f;
    float pan_x = 0.0f;
    float pan_y = 0.0f;
    bool hdr_toggle = false;
    renderer.set_hdr_tone_mapping(hdr_toggle);

    auto last_title_update = std::chrono::high_resolution_clock::now();
    auto start_time_global = std::chrono::high_resolution_clock::now();
    auto last_user_activity = std::chrono::high_resolution_clock::now();

    bool space_pressed_prev = false;
    bool h_pressed_prev = false;
    bool c_pressed_prev = false;
    bool f_pressed_prev = false;
    bool p_pressed_prev = false;
    bool w_pressed_prev = false;
    bool v_pressed_prev = false;
    bool vsync_enabled = true;
    bool left_pressed_prev = false;
    bool right_pressed_prev = false;
    bool j_pressed_prev = false;
    bool k_pressed_prev = false;
    bool o_pressed_prev = false;
    bool g_pressed_prev = false;
    bool l_pressed_prev = false;
    bool s_pressed_prev = false;
    bool d_pressed_prev = false;
    bool a_pressed_prev = false;
    bool m_pressed_prev = false;
    bool left_bracket_prev = false;
    bool right_bracket_prev = false;
    bool bs_prev = false;
    bool f12_pressed_prev = false;
    bool z_pressed_prev = false;
    bool x_pressed_prev = false;
    bool k1_prev = false, k2_prev = false, k3_prev = false, k4_prev = false, k5_prev = false, k6_prev = false, k0_prev = false;
    double subtitle_delay_sec = 0.0;
    bool n_pressed_prev = false;
    bool b_pressed_prev = false;
    bool esc_pressed_prev = false;
    bool mouse_left_prev = false;
    bool is_scrubbing = false;
    bool eof_handled = false;
    double last_mouse_x = 0, last_mouse_y = 0;
    auto last_video_click_time = std::chrono::high_resolution_clock::time_point{};

    while (!glfwWindowShouldClose(window)) {
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        if (width > 0 && height > 0) {
            glViewport(0, 0, width, height);
        }
        auto now = std::chrono::high_resolution_clock::now();

        // Cursor coordinates
        double mx = 0, my = 0;
        glfwGetCursorPos(window, &mx, &my);
        int win_w = 0, win_h = 0;
        glfwGetWindowSize(window, &win_w, &win_h);
        if (win_w > 0 && win_h > 0) {
            mx = mx * (static_cast<double>(width) / win_w);
            my = my * (static_cast<double>(height) / win_h);
        }
        if (mx != last_mouse_x || my != last_mouse_y) {
            last_mouse_x = mx;
            last_mouse_y = my;
            last_user_activity = now;

            // If a top menu is open, hovering over another category switches to it (only if not fullscreen)
            if (active_menu_idx >= 0 && !is_fullscreen) {
                int hovered_menu = renderer.hit_test_vlc_menu_bar(width, height, mx, my);
                if (hovered_menu >= 0 && hovered_menu != active_menu_idx) {
                    active_menu_idx = hovered_menu;
                }
            }
            // If context menu is open, hovering over another category switches to it
            if (context_menu_active) {
                int hovered_cat = renderer.hit_test_vlc_context_menu_category(width, height, mx, my, context_menu_x, context_menu_y);
                if (hovered_cat >= 0) {
                    context_submenu_idx = hovered_cat;
                }
            }
        }

        auto handle_vlc_menu_action_fn = [&](VlcMenuAction action) {
            switch (action) {
                case VlcMenuAction::MEDIA_OPEN_FILE: {
                    std::string file = open_file_dialog_fn();
                    if (!file.empty()) {
                        playlist_files = { file };
                        current_file_idx = 0;
                        if (load_playlist_file_fn(0)) {
                            app_state = AppState::PLAYING;
                        }
                    }
                    break;
                }
                case VlcMenuAction::MEDIA_OPEN_FOLDER: {
                    std::string dir = open_folder_dialog_fn();
                    if (!dir.empty()) {
                        if (scan_folder_fn(dir)) {
                            gallery_scroll_offset = 0;
                            app_state = AppState::FOLDER_GALLERY;
                        }
                    }
                    break;
                }
                case VlcMenuAction::MEDIA_QUIT: {
                    if (!video_filename.empty() && current_file_idx < playlist_files.size()) {
                        ResumeManager::get_instance().save_position(playlist_files[current_file_idx], player.get_current_time(), player.get_duration());
                    }
                    glfwSetWindowShouldClose(window, true);
                    break;
                }
                case VlcMenuAction::PLAYBACK_TOGGLE_PAUSE: {
                    if (app_state == AppState::PLAYING) {
                        player.toggle_pause();
                    }
                    break;
                }
                case VlcMenuAction::PLAYBACK_FORWARD: {
                    if (app_state == AppState::PLAYING) {
                        player.seek_relative(5.0);
                        osd_notification = "Seek: +5s";
                        osd_notification_time = now;
                    }
                    break;
                }
                case VlcMenuAction::PLAYBACK_REWIND: {
                    if (app_state == AppState::PLAYING) {
                        player.seek_relative(-5.0);
                        osd_notification = "Seek: -5s";
                        osd_notification_time = now;
                    }
                    break;
                }
                case VlcMenuAction::AUDIO_VOL_UP: {
                    osd_notification = "Volume: +10%";
                    osd_notification_time = now;
                    break;
                }
                case VlcMenuAction::AUDIO_VOL_DOWN: {
                    osd_notification = "Volume: -10%";
                    osd_notification_time = now;
                    break;
                }
                case VlcMenuAction::AUDIO_MUTE: {
                    osd_notification = "Audio Mute Toggled";
                    osd_notification_time = now;
                    break;
                }
                case VlcMenuAction::VIDEO_FULLSCREEN: {
                    toggle_fullscreen_fn(window);
                    break;
                }
                case VlcMenuAction::VIDEO_ASPECT_RATIO: {
                    auto cur_ratio = renderer.get_aspect_ratio_mode();
                    using AR = ShaderRenderer::AspectRatioMode;
                    AR next_ratio = AR::AUTO;
                    if (cur_ratio == AR::AUTO) next_ratio = AR::RATIO_16_9;
                    else if (cur_ratio == AR::RATIO_16_9) next_ratio = AR::RATIO_4_3;
                    else if (cur_ratio == AR::RATIO_4_3) next_ratio = AR::RATIO_21_9;
                    else if (cur_ratio == AR::RATIO_21_9) next_ratio = AR::RATIO_1_1;
                    else if (cur_ratio == AR::RATIO_1_1) next_ratio = AR::RATIO_FILL;
                    else next_ratio = AR::AUTO;
                    renderer.set_aspect_ratio_mode(next_ratio);
                    osd_notification = "Aspect: " + renderer.get_aspect_ratio_name();
                    osd_notification_time = now;
                    break;
                }
                case VlcMenuAction::SUBTITLE_TOGGLE: {
                    show_subtitles = !show_subtitles;
                    osd_notification = show_subtitles ? "Subtitles: ON" : "Subtitles: OFF";
                    osd_notification_time = now;
                    break;
                }
                case VlcMenuAction::SUBTITLE_DELAY_MINUS: {
                    subtitle_delay_sec -= 0.1;
                    int ms = static_cast<int>(std::round(subtitle_delay_sec * 1000.0));
                    osd_notification = std::string("Sub Delay: ") + (ms >= 0 ? "+" : "") + std::to_string(ms) + " ms";
                    osd_notification_time = now;
                    break;
                }
                case VlcMenuAction::SUBTITLE_DELAY_PLUS: {
                    subtitle_delay_sec += 0.1;
                    int ms = static_cast<int>(std::round(subtitle_delay_sec * 1000.0));
                    osd_notification = std::string("Sub Delay: ") + (ms >= 0 ? "+" : "") + std::to_string(ms) + " ms";
                    osd_notification_time = now;
                    break;
                }
                case VlcMenuAction::TOOLS_STATS: {
                    show_stats = !show_stats;
                    osd_notification = show_stats ? "Telemetry: ON" : "Telemetry: OFF";
                    osd_notification_time = now;
                    break;
                }
                case VlcMenuAction::VIEW_GALLERY: {
                    if (!folder_media_items.empty()) {
                        app_state = AppState::FOLDER_GALLERY;
                    } else {
                        app_state = AppState::WELCOME;
                    }
                    break;
                }
                case VlcMenuAction::HELP_ABOUT: {
                    osd_notification = "VMP v0.0.2-beta - Video Max Player";
                    osd_notification_time = now;
                    break;
                }
                default:
                    break;
            }
        };

        auto check_vlc_menu_click_fn = [&]() -> bool {
            if (context_menu_active) {
                if (context_submenu_idx >= 0) {
                    float card_w = 170.0f;
                    float card_h = 8.0f * 26.0f + 8.0f;
                    float root_x = std::max(4.0f, std::min(context_menu_x, static_cast<float>(width) - card_w - 10.0f));
                    float root_y = std::max(4.0f, std::min(context_menu_y, static_cast<float>(height) - card_h - 10.0f));
                    float sub_x = root_x + card_w + 2.0f;
                    if (sub_x + 260.0f > static_cast<float>(width)) sub_x = root_x - 262.0f;
                    float sub_y = root_y + 4.0f + static_cast<float>(context_submenu_idx) * 26.0f;

                    VlcMenuAction action = renderer.hit_test_vlc_dropdown(width, height, mx, my, context_submenu_idx, sub_x, sub_y);
                    if (action != VlcMenuAction::NONE) {
                        handle_vlc_menu_action_fn(action);
                        context_menu_active = false;
                        context_submenu_idx = -1;
                        return true;
                    }
                }

                int cat = renderer.hit_test_vlc_context_menu_category(width, height, mx, my, context_menu_x, context_menu_y);
                if (cat >= 0) {
                    context_submenu_idx = cat;
                    return true;
                }

                context_menu_active = false;
                context_submenu_idx = -1;
                return true;
            }

            if (is_fullscreen) return false;
            int clicked_top = renderer.hit_test_vlc_menu_bar(width, height, mx, my);
            if (clicked_top >= 0) {
                if (active_menu_idx == clicked_top) {
                    active_menu_idx = -1;
                } else {
                    active_menu_idx = clicked_top;
                }
                return true;
            }
            if (active_menu_idx >= 0) {
                VlcMenuAction action = renderer.hit_test_vlc_dropdown(width, height, mx, my, active_menu_idx);
                if (action != VlcMenuAction::NONE) {
                    handle_vlc_menu_action_fn(action);
                    active_menu_idx = -1;
                    return true;
                }
                if (renderer.is_mouse_inside_dropdown(width, height, mx, my, active_menu_idx)) {
                    return true;
                }
                active_menu_idx = -1;
                return true;
            }
            return false;
        };

        // Global Fullscreen Toggle Key Handler (F, F11, Alt+Enter)
        bool f_key_pressed = (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS || 
                              glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS ||
                              ((glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS) && glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS));
        if (f_key_pressed && !f_pressed_prev) {
            toggle_fullscreen_fn(window);
            f_pressed_prev = true;
        } else if (!f_key_pressed) {
            f_pressed_prev = false;
        }

        // Drag & Drop Files Processing
        std::vector<std::string> dropped_to_process;
        {
            std::lock_guard<std::mutex> lock(g_dropped_mutex);
            if (!g_dropped_files.empty()) {
                dropped_to_process = g_dropped_files;
                g_dropped_files.clear();
            }
        }
        for (const auto& d_path : dropped_to_process) {
            if (std::filesystem::is_directory(d_path)) {
                if (scan_folder_fn(d_path)) {
                    gallery_scroll_offset = 0;
                    app_state = AppState::FOLDER_GALLERY;
                }
            } else {
                std::string ext = "";
                size_t dot = d_path.find_last_of('.');
                if (dot != std::string::npos) {
                    ext = d_path.substr(dot);
                    for (char& c : ext) c = std::tolower(c);
                }
                if (ext == ".srt" || ext == ".ass" || ext == ".ssa") {
                    SubtitleTrack track;
                    track.title = std::filesystem::path(d_path).filename().string();
                    track.filepath = d_path;
                    track.entries = SubtitleParser::parse_file(d_path);
                    available_sub_tracks.insert(available_sub_tracks.begin(), track);
                    current_sub_track_idx = 0;
                    subtitle_entries = track.entries;
                    show_subtitles = true;
                    osd_notification = "Subtitles Loaded: " + track.title;
                    osd_notification_time = now;
                    last_user_activity = now;
                    std::cout << "\n[VMP Subtitles] Dropped subtitle file loaded: " << track.title 
                              << " (" << track.entries.size() << " entries)" << std::endl;
                } else if (is_image_file(d_path)) {
                    osd_notification = "Images not supported (Video only)";
                    osd_notification_time = now;
                    last_user_activity = now;
                    std::cout << "\n[VMP Engine] Dropped file is an image: " << d_path 
                              << " (rejected: VMP is dedicated to video only)." << std::endl;
                } else {
                    playlist_files = { d_path };
                    current_file_idx = 0;
                    if (load_playlist_file_fn(0)) {
                        app_state = AppState::PLAYING;
                    } else {
                        osd_notification = "Failed to load video file";
                        osd_notification_time = now;
                    }
                    last_user_activity = now;
                }
            }
        }

        // Open Folder dialog shortcut (Ctrl+O or O)
        bool o_pressed_now = (glfwGetKey(window, GLFW_KEY_O) == GLFW_PRESS);
        if (o_pressed_now && !o_pressed_prev) {
            std::string dir = open_folder_dialog_fn();
            if (!dir.empty()) {
                if (scan_folder_fn(dir)) {
                    gallery_scroll_offset = 0;
                    app_state = AppState::FOLDER_GALLERY;
                } else {
                    osd_notification = "No media files in folder";
                    osd_notification_time = now;
                }
            }
            last_user_activity = now;
            o_pressed_prev = true;
        } else if (!o_pressed_now) {
            o_pressed_prev = false;
        }

        // Toggle Folder Gallery shortcut (G key)
        bool g_pressed_now = (glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS);
        if (g_pressed_now && !g_pressed_prev) {
            if (!folder_media_items.empty()) {
                if (app_state == AppState::PLAYING) {
                    player.set_paused(true);
                    app_state = AppState::FOLDER_GALLERY;
                } else if (app_state == AppState::FOLDER_GALLERY) {
                    app_state = AppState::PLAYING;
                    player.set_paused(false);
                }
            }
            g_pressed_prev = true;
        } else if (!g_pressed_now) {
            g_pressed_prev = false;
        }

        // Mouse scroll in Gallery mode
        if (app_state == AppState::FOLDER_GALLERY) {
            if (std::abs(g_scroll_y) > 0.01) {
                float pad_x = 24.0f;
                float gap_x = 18.0f;
                float avail_w = static_cast<float>(width) - 2.0f * pad_x;
                int num_cols = std::max(2, std::min(6, static_cast<int>((avail_w + gap_x) / 240.0f)));
                int num_rows = (static_cast<int>(folder_media_items.size()) + num_cols - 1) / num_cols;

                if (g_scroll_y > 0) gallery_scroll_offset = std::max(0, gallery_scroll_offset - 1);
                else gallery_scroll_offset = std::min(std::max(0, num_rows - 1), gallery_scroll_offset + 1);
                g_scroll_y = 0.0;
            }
        }

        // Handle Welcome Screen
        if (app_state == AppState::WELCOME) {
            float scale = std::clamp(std::min(static_cast<float>(width) / 1280.0f, static_cast<float>(height) / 720.0f), 0.85f, 2.0f);
            float card_w = std::min(static_cast<float>(width) - 40.0f, 680.0f * scale);
            float card_h = std::min(static_cast<float>(height) - 40.0f, 380.0f * scale);
            float card_x = (static_cast<float>(width) - card_w) / 2.0f;
            float card_y = (static_cast<float>(height) - card_h) / 2.0f;

            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (!mouse_left_prev) {
                    if (check_vlc_menu_click_fn()) {
                        // Handled by VLC menu bar
                    } else {
                        float btn_w = 200.0f * scale;
                        float btn_h = 36.0f * scale;
                        float btn_x = card_x + (card_w - btn_w) / 2.0f;
                        float btn_y = card_y + card_h - (60.0f * scale);

                        bool clicked_btn = (mx >= btn_x && mx <= btn_x + btn_w && my >= btn_y && my <= btn_y + btn_h);
                        bool clicked_card = (mx >= card_x && mx <= card_x + card_w && my >= card_y && my <= card_y + card_h);

                        if (clicked_btn || clicked_card) {
                            std::string dir = open_folder_dialog_fn();
                            if (!dir.empty()) {
                                if (scan_folder_fn(dir)) {
                                    gallery_scroll_offset = 0;
                                    app_state = AppState::FOLDER_GALLERY;
                                }
                            }
                        } else {
                            // Double-click on background area toggles fullscreen
                            double click_delta_ms = std::chrono::duration<double, std::milli>(now - last_video_click_time).count();
                            if (click_delta_ms > 40.0 && click_delta_ms < 350.0) {
                                toggle_fullscreen_fn(window);
                                last_video_click_time = std::chrono::high_resolution_clock::time_point{};
                            } else {
                                last_video_click_time = now;
                            }
                        }
                    }
                    mouse_left_prev = true;
                }
            } else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_RELEASE) {
                mouse_left_prev = false;
            }

            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
                if (!mouse_right_prev) {
                    active_menu_idx = -1;
                    context_menu_active = true;
                    context_menu_x = static_cast<float>(mx);
                    context_menu_y = static_cast<float>(my);
                    context_submenu_idx = -1;
                    mouse_right_prev = true;
                }
            } else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_RELEASE) {
                mouse_right_prev = false;
            }

            renderer.render_welcome_screen(width, height, mx, my);
            if (!is_fullscreen) {
                renderer.render_vlc_menu_bar(width, height, mx, my, active_menu_idx, 1.0f);
                if (active_menu_idx >= 0) {
                    renderer.render_vlc_dropdown(width, height, mx, my, active_menu_idx);
                }
            }
            if (context_menu_active) {
                renderer.render_vlc_context_menu(width, height, mx, my, context_menu_x, context_menu_y, context_submenu_idx);
            }
            glfwSwapBuffers(window);
            glfwPollEvents();

            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                if (context_menu_active) {
                    context_menu_active = false;
                    context_submenu_idx = -1;
                } else if (active_menu_idx >= 0) {
                    active_menu_idx = -1;
                } else if (is_fullscreen) {
                    toggle_fullscreen_fn(window);
                } else {
                    glfwSetWindowShouldClose(window, true);
                }
            }
            continue;
        }

        // Handle Folder Media Gallery
        if (app_state == AppState::FOLDER_GALLERY) {
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (!mouse_left_prev) {
                    if (check_vlc_menu_click_fn()) {
                        // Handled by VLC menu bar
                    } else {
                        // Check "SELECT FOLDER" button on top right (adjusted below menu bar if not fullscreen)
                        float menu_bar_h = is_fullscreen ? 0.0f : 28.0f;
                        float header_y = menu_bar_h;
                        float header_h = 56.0f;
                        float chg_btn_w = 170.0f;
                        float chg_btn_h = 32.0f;
                        float chg_btn_x = width - chg_btn_w - 24.0f;
                        float chg_btn_y = header_y + 12.0f;
                        if (mx >= chg_btn_x && mx <= chg_btn_x + chg_btn_w && my >= chg_btn_y && my <= chg_btn_y + chg_btn_h) {
                            std::string dir = open_folder_dialog_fn();
                            if (!dir.empty()) {
                                if (scan_folder_fn(dir)) {
                                    gallery_scroll_offset = 0;
                                }
                            }
                        } else {
                            // Responsive grid hit-testing
                            float grid_top = header_y + header_h + 16.0f;
                            float pad_x = 24.0f;
                            float gap_x = 18.0f;
                            float gap_y = 20.0f;
                            float avail_w = static_cast<float>(width) - 2.0f * pad_x;

                            int num_cols = std::max(2, std::min(6, static_cast<int>((avail_w + gap_x) / 240.0f)));
                            float card_w = (avail_w - (num_cols - 1) * gap_x) / num_cols;
                            float thumb_h = card_w * (9.0f / 16.0f);
                            float text_h = 44.0f;
                            float card_h = thumb_h + text_h;

                            for (size_t i = 0; i < folder_media_items.size(); i++) {
                                int r = static_cast<int>(i) / num_cols;
                                int c = static_cast<int>(i) % num_cols;
                                int row_on_screen = r - gallery_scroll_offset;
                                if (row_on_screen < 0) continue;

                                float card_x = pad_x + c * (card_w + gap_x);
                                float card_y = grid_top + row_on_screen * (card_h + gap_y);
                                if (card_y + card_h > height + 50.0f) break;

                                if (mx >= card_x && mx <= card_x + card_w && my >= card_y && my <= card_y + card_h) {
                                    current_file_idx = i;
                                    if (load_playlist_file_fn(current_file_idx)) {
                                        app_state = AppState::PLAYING;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    mouse_left_prev = true;
                }
            } else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_RELEASE) {
                mouse_left_prev = false;
            }

            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
                if (!mouse_right_prev) {
                    active_menu_idx = -1;
                    context_menu_active = true;
                    context_menu_x = static_cast<float>(mx);
                    context_menu_y = static_cast<float>(my);
                    context_submenu_idx = -1;
                    mouse_right_prev = true;
                }
            } else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_RELEASE) {
                mouse_right_prev = false;
            }

            renderer.render_folder_gallery(width, height, mx, my, current_folder_path, folder_media_items, gallery_scroll_offset, is_fullscreen);
            if (!is_fullscreen) {
                renderer.render_vlc_menu_bar(width, height, mx, my, active_menu_idx, 1.0f);
                if (active_menu_idx >= 0) {
                    renderer.render_vlc_dropdown(width, height, mx, my, active_menu_idx);
                }
            }
            if (context_menu_active) {
                renderer.render_vlc_context_menu(width, height, mx, my, context_menu_x, context_menu_y, context_submenu_idx);
            }
            glfwSwapBuffers(window);
            glfwPollEvents();

            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                if (context_menu_active) {
                    context_menu_active = false;
                    context_submenu_idx = -1;
                } else if (active_menu_idx >= 0) {
                    active_menu_idx = -1;
                } else if (is_fullscreen) {
                    toggle_fullscreen_fn(window);
                } else {
                    app_state = AppState::WELCOME;
                }
            }
            continue;
        }

        // Calculate OSD UI Fade Alpha (Stays visible for 2.5s, then smooth 0.5s fade out)
        double inactive_sec = std::chrono::duration<double>(now - last_user_activity).count();
        float ui_alpha = 1.0f;
        if (inactive_sec > 2.5) {
            ui_alpha = std::max(0.0f, 1.0f - static_cast<float>((inactive_sec - 2.5) / 0.5));
        }

        // Show/hide mouse cursor based on UI activity/visibility
        if (ui_alpha < 0.05f) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
        } else {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        // Mouse Drag Scrubbing & Click Controls
        float center_y = height - 40.0f;
        float icon_size = 22.0f;

        float replay_x = 24.0f;
        float play_x = 58.0f;
        float forward_x = 92.0f;
        float icon_y = center_y - (icon_size / 2.0f);

        bool has_hours = (player.get_duration() >= 3600.0);
        std::string full_time_str = format_time(player.get_current_time(), has_hours) + " / " + format_time(player.get_duration(), has_hours);
        float time_x = forward_x + icon_size + 14.0f;
        float time_text_w = renderer.get_text_width(full_time_str, 14.0f);

        float bar_x = time_x + time_text_w + 15.0f;
        float fs_x = width - 42.0f;
        float fs_y = center_y - (icon_size / 2.0f);

        float back_btn_w = 110.0f;
        float back_btn_h = 32.0f;
        float back_btn_x = width - back_btn_w - 20.0f;
        float back_btn_y = is_fullscreen ? 16.0f : 36.0f;

        float stats_x = back_btn_x - 38.0f;
        float stats_y = back_btn_y + 5.0f;

        float bar_w = (fs_x - 15.0f) - bar_x;
        float bar_y = center_y;

        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            last_user_activity = now;
            if (!mouse_right_prev) {
                active_menu_idx = -1;
                context_menu_active = true;
                context_menu_x = static_cast<float>(mx);
                context_menu_y = static_cast<float>(my);
                context_submenu_idx = -1;
                mouse_right_prev = true;
            }
        } else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_RELEASE) {
            mouse_right_prev = false;
        }

        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
            last_user_activity = now;
            if (!mouse_left_prev) {
                if (check_vlc_menu_click_fn()) {
                    // Handled by VLC menu bar or context menu
                }
                // Check click on BACK button (at top-right, only in windowed mode)
                else if (!is_fullscreen && mx >= back_btn_x && mx <= back_btn_x + back_btn_w && my >= back_btn_y && my <= back_btn_y + back_btn_h) {
                    if (!video_filename.empty() && current_file_idx < playlist_files.size()) {
                        ResumeManager::get_instance().save_position(playlist_files[current_file_idx], player.get_current_time(), player.get_duration());
                    }
                    player.close_file();
                    renderer.clear_video_frame();
                    thumb_gen.stop();
                    video_filename.clear();
                    if (!folder_media_items.empty()) {
                        app_state = AppState::FOLDER_GALLERY;
                    } else {
                        app_state = AppState::WELCOME;
                    }
                }
                // Check click on Fullscreen button icon (at bottom-right)
                else if (mx >= fs_x - 12.0f && mx <= fs_x + icon_size + 12.0f && my >= fs_y - 12.0f && my <= fs_y + icon_size + 12.0f) {
                    toggle_fullscreen_fn(window);
                }
                // Check click on Stats button icon (at top-right, left of BACK, only in windowed mode)
                else if (!is_fullscreen && mx >= stats_x - 10.0f && mx <= stats_x + icon_size + 10.0f && my >= stats_y - 10.0f && my <= stats_y + icon_size + 10.0f) {
                    show_stats = !show_stats;
                    std::cout << "[VMP Engine] Toggle Stats Display: " << (show_stats ? "ON" : "OFF") << std::endl;
                }
                // Check click on Replay 5s button icon
                else if (mx >= replay_x - 10.0f && mx <= replay_x + icon_size + 10.0f && my >= icon_y - 12.0f && my <= icon_y + icon_size + 12.0f) {
                    player.seek_relative(-5.0);
                    osd_notification = "Seek: -5s";
                    osd_notification_time = now;
                }
                // Check click on Play/Pause button icon
                else if (mx >= play_x - 10.0f && mx <= play_x + icon_size + 10.0f && my >= icon_y - 12.0f && my <= icon_y + icon_size + 12.0f) {
                    player.toggle_pause();
                }
                // Check click on Forward 5s button icon
                else if (mx >= forward_x - 10.0f && mx <= forward_x + icon_size + 10.0f && my >= icon_y - 12.0f && my <= icon_y + icon_size + 12.0f) {
                    player.seek_relative(5.0);
                    osd_notification = "Seek: +5s";
                    osd_notification_time = now;
                }
                // Check if click started on progress bar timeline area
                else if (mx >= bar_x - 10.0f && mx <= bar_x + bar_w + 10.0f && my >= bar_y - 20.0f && my <= bar_y + 20.0f) {
                    is_scrubbing = true;
                }
                // Click in the middle of the screen (main video area) -> Double Click for Fullscreen, Single Click for Play/Pause
                else if (my >= 60.0f && my < height - 70.0f) {
                    double click_delta_ms = std::chrono::duration<double, std::milli>(now - last_video_click_time).count();
                    if (click_delta_ms > 40.0 && click_delta_ms < 350.0) {
                        toggle_fullscreen_fn(window);
                        last_video_click_time = std::chrono::high_resolution_clock::time_point{};
                    } else {
                        player.toggle_pause();
                        last_video_click_time = now;
                    }
                }
                mouse_left_prev = true;
            }

            if (is_scrubbing) {
                double ratio = std::max(0.0, std::min(1.0, (mx - bar_x) / bar_w));
                player.seek_to_ratio(ratio);
            }
        } else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_RELEASE) {
            mouse_left_prev = false;
            is_scrubbing = false;
        }

        bool is_hover = (mx >= bar_x - 10.0f && mx <= bar_x + bar_w + 10.0f && my >= bar_y - 20.0f && my <= bar_y + 20.0f);
        if ((is_hover || is_scrubbing) && player.get_duration() > 0.0) {
            double hover_ratio = std::max(0.0, std::min(1.0, (mx - bar_x) / bar_w));
            double hover_sec = hover_ratio * player.get_duration();
            std::vector<uint8_t> thumb_rgb;
            int tw = 0, th = 0;
            if (thumb_gen.get_thumbnail_at_time(hover_sec, thumb_rgb, tw, th)) {
                renderer.upload_thumbnail_frame(thumb_rgb.data(), tw, th);
            }
        }

        // Mouse Scroll Wheel Volume Control (0% - 150%)
        if (std::abs(g_scroll_y) > 0.01) {
            float cur_vol = player.get_volume();
            float new_vol = std::max(0.0f, std::min(1.5f, cur_vol + static_cast<float>(g_scroll_y * 0.05)));
            player.set_volume(new_vol);
            g_scroll_y = 0.0;
            int vol_pct = static_cast<int>(std::round(new_vol * 100));
            std::string vol_icon = (vol_pct == 0) ? "🔇" : (vol_pct > 100 ? "⚡" : "🔊");
            osd_notification = "Volume: " + std::to_string(vol_pct) + "% " + vol_icon;
            osd_notification_time = now;
            last_user_activity = now;
        }

        // Periodic auto-save of current playback position every 3 seconds
        if (std::chrono::duration<double>(now - last_resume_save).count() >= 3.0) {
            if (!playlist_files.empty() && current_file_idx < playlist_files.size()) {
                ResumeManager::get_instance().save_position(playlist_files[current_file_idx], player.get_current_time(), player.get_duration());
            }
            last_resume_save = now;
        }

        // Keyboard Controls
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS && !space_pressed_prev) {
            player.toggle_pause();
            osd_notification = player.is_paused() ? "Pause" : "Play";
            osd_notification_time = now;
            last_user_activity = now;
            space_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_RELEASE) {
            space_pressed_prev = false;
        }

        // Fullscreen Controls (F or F11 key)
        if ((glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS) && !f_pressed_prev) {
            toggle_fullscreen_fn(window);
            last_user_activity = now;
            f_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_F) == GLFW_RELEASE && glfwGetKey(window, GLFW_KEY_F11) == GLFW_RELEASE) {
            f_pressed_prev = false;
        }

        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS && !left_pressed_prev) {
            player.seek_relative(-5.0);
            osd_notification = "Seek: -5s";
            osd_notification_time = now;
            last_user_activity = now;
            left_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_RELEASE) {
            left_pressed_prev = false;
        }

        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS && !right_pressed_prev) {
            player.seek_relative(5.0);
            osd_notification = "Seek: +5s";
            osd_notification_time = now;
            last_user_activity = now;
            right_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_RELEASE) {
            right_pressed_prev = false;
        }

        if (glfwGetKey(window, GLFW_KEY_J) == GLFW_PRESS && !j_pressed_prev) {
            player.seek_relative(-10.0);
            osd_notification = "Seek: -10s";
            osd_notification_time = now;
            last_user_activity = now;
            j_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_J) == GLFW_RELEASE) {
            j_pressed_prev = false;
        }

        if (glfwGetKey(window, GLFW_KEY_K) == GLFW_PRESS && !k_pressed_prev) {
            player.toggle_pause();
            osd_notification = player.is_paused() ? "Pause" : "Play";
            osd_notification_time = now;
            last_user_activity = now;
            k_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_K) == GLFW_RELEASE) {
            k_pressed_prev = false;
        }
        
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) { current_zoom += 0.02f; last_user_activity = now; }
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) { current_zoom = std::max(0.2f, current_zoom - 0.02f); last_user_activity = now; }
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) { current_zoom = 1.0f; pan_x = 0.0f; pan_y = 0.0f; last_user_activity = now; }

        bool shift_held = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);

        if (glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS && !h_pressed_prev) {
            if (shift_held) {
                // Shift+H: Cycle Hardware Acceleration Mode (AUTO -> FORCE_CPU -> FORCE_HW -> AUTO)
                auto cur_hw = player.get_hw_accel_mode();
                if (cur_hw == VideoPlayer::HWAccelMode::AUTO) {
                    player.set_hw_accel_mode(VideoPlayer::HWAccelMode::FORCE_CPU);
                    osd_notification = "Decoder: CPU Multi-Threaded Engine (AVX2)";
                } else if (cur_hw == VideoPlayer::HWAccelMode::FORCE_CPU) {
                    player.set_hw_accel_mode(VideoPlayer::HWAccelMode::FORCE_HW);
                    osd_notification = "Decoder: Hardware Acceleration (VAAPI/CUDA)";
                } else {
                    player.set_hw_accel_mode(VideoPlayer::HWAccelMode::AUTO);
                    osd_notification = "Decoder: AUTO (Smart Routing)";
                }
                osd_notification_time = now;
                std::cout << "\n[VMP Engine] " << osd_notification << std::endl;
                if (current_file_idx < playlist_files.size()) {
                    load_playlist_file_fn(current_file_idx);
                }
            } else {
                hdr_toggle = !hdr_toggle;
                renderer.set_hdr_tone_mapping(hdr_toggle);
                osd_notification = hdr_toggle ? "HDR: ON" : "HDR: OFF";
                osd_notification_time = now;
                std::cout << "\n[VMP Engine] HDR Tone Mapping: " << (hdr_toggle ? "ENABLED" : "DISABLED") << std::endl;
            }
            last_user_activity = now;
            h_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_H) == GLFW_RELEASE) {
            h_pressed_prev = false;
        }

        // GPU Contrast-Adaptive Sharpening (CAS) Clarity toggle (C key)
        if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS && !c_pressed_prev) {
            bool sharp_toggle = !renderer.is_sharpness_filter_enabled();
            renderer.set_sharpness_filter(sharp_toggle);
            osd_notification = sharp_toggle ? "CAS Clarity: ON" : "CAS Clarity: OFF";
            osd_notification_time = now;
            std::cout << "\n[VMP Engine] CAS GPU Clarity Filter: " << (sharp_toggle ? "ENABLED (Ultra Sharp)" : "DISABLED") << std::endl;
            last_user_activity = now;
            c_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_C) == GLFW_RELEASE) {
            c_pressed_prev = false;
        }

        if (glfwGetKey(window, GLFW_KEY_P) == GLFW_PRESS && !p_pressed_prev) {
            ShaderRenderer::RenderMode next_mode;
            std::string mode_name;
            switch (renderer.get_render_mode()) {
                case ShaderRenderer::RenderMode::FIT:
                    next_mode = ShaderRenderer::RenderMode::PIXEL_PERFECT;
                    mode_name = "Pixel-Perfect";
                    break;
                case ShaderRenderer::RenderMode::PIXEL_PERFECT:
                    next_mode = ShaderRenderer::RenderMode::STRETCH;
                    mode_name = "Stretch";
                    break;
                case ShaderRenderer::RenderMode::STRETCH:
                default:
                    next_mode = ShaderRenderer::RenderMode::FIT;
                    mode_name = "Fit";
                    break;
            }
            renderer.set_render_mode(next_mode);
            osd_notification = "Mode: " + mode_name;
            osd_notification_time = now;
            std::cout << "\n[VMP Engine] Presentation Mode changed to: " << mode_name << std::endl;
            last_user_activity = now;
            p_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_P) == GLFW_RELEASE) {
            p_pressed_prev = false;
        }

        // Aspect Ratio Override cycle (W key or Shift+A)
        bool w_pressed_now = (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS || (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS && shift_held));
        if (w_pressed_now && !w_pressed_prev) {
            ShaderRenderer::AspectRatioMode cur_aspect = renderer.get_aspect_ratio_mode();
            ShaderRenderer::AspectRatioMode next_aspect;
            switch (cur_aspect) {
                case ShaderRenderer::AspectRatioMode::AUTO:
                    next_aspect = ShaderRenderer::AspectRatioMode::RATIO_16_9;
                    break;
                case ShaderRenderer::AspectRatioMode::RATIO_16_9:
                    next_aspect = ShaderRenderer::AspectRatioMode::RATIO_4_3;
                    break;
                case ShaderRenderer::AspectRatioMode::RATIO_4_3:
                    next_aspect = ShaderRenderer::AspectRatioMode::RATIO_21_9;
                    break;
                case ShaderRenderer::AspectRatioMode::RATIO_21_9:
                    next_aspect = ShaderRenderer::AspectRatioMode::RATIO_1_1;
                    break;
                case ShaderRenderer::AspectRatioMode::RATIO_1_1:
                    next_aspect = ShaderRenderer::AspectRatioMode::RATIO_FILL;
                    break;
                case ShaderRenderer::AspectRatioMode::RATIO_FILL:
                default:
                    next_aspect = ShaderRenderer::AspectRatioMode::AUTO;
                    break;
            }
            renderer.set_aspect_ratio_mode(next_aspect);
            osd_notification = "Aspect: " + renderer.get_aspect_ratio_name();
            osd_notification_time = now;
            std::cout << "\n[VMP Engine] Aspect Ratio Override: " << renderer.get_aspect_ratio_name() << std::endl;
            last_user_activity = now;
            w_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_W) == GLFW_RELEASE && (!shift_held || glfwGetKey(window, GLFW_KEY_A) == GLFW_RELEASE)) {
            w_pressed_prev = false;
        }

        // VSync / Benchmark Mode toggle (V key)
        if (glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS && !v_pressed_prev) {
            vsync_enabled = !vsync_enabled;
            glfwSwapInterval(vsync_enabled ? 1 : 0);
            osd_notification = vsync_enabled ? "VSync: ON" : "VSync: OFF (Benchmark)";
            osd_notification_time = now;
            std::cout << "\n[VMP Engine] VSync Mode: " << (vsync_enabled ? "ENABLED (Smooth)" : "DISABLED (Benchmark)") << std::endl;
            last_user_activity = now;
            v_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_V) == GLFW_RELEASE) {
            v_pressed_prev = false;
        }

        // Loop toggle (L key)
        if (glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS && !l_pressed_prev) {
            loop_mode = !loop_mode;
            osd_notification = loop_mode ? "Loop: ON" : "Loop: OFF";
            osd_notification_time = now;
            std::cout << "\n[VMP Engine] Video Loop Mode: " << (loop_mode ? "ENABLED" : "DISABLED") << std::endl;
            last_user_activity = now;
            l_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_L) == GLFW_RELEASE) {
            l_pressed_prev = false;
        }

        // Subtitles toggle (S key) & Cycle (Shift+S or D key)
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS && !s_pressed_prev) {
            if (shift_held) {
                if (!available_sub_tracks.empty()) {
                    current_sub_track_idx = (current_sub_track_idx + 1) % available_sub_tracks.size();
                    subtitle_entries = available_sub_tracks[current_sub_track_idx].entries;
                    osd_notification = "Sub: " + available_sub_tracks[current_sub_track_idx].title;
                    osd_notification_time = now;
                    std::cout << "\n[VMP Subtitles] Switched to track: " << available_sub_tracks[current_sub_track_idx].title << std::endl;
                }
            } else {
                show_subtitles = !show_subtitles;
                osd_notification = show_subtitles ? "Subtitles: ON" : "Subtitles: OFF";
                osd_notification_time = now;
                std::cout << "\n[VMP Engine] Subtitles Display: " << (show_subtitles ? "ON" : "OFF") << std::endl;
            }
            last_user_activity = now;
            s_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_S) == GLFW_RELEASE) {
            s_pressed_prev = false;
        }

        // Subtitle track cycle dedicated key (D key)
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS && !d_pressed_prev) {
            if (!available_sub_tracks.empty()) {
                current_sub_track_idx = (current_sub_track_idx + 1) % available_sub_tracks.size();
                subtitle_entries = available_sub_tracks[current_sub_track_idx].entries;
                osd_notification = "Sub: " + available_sub_tracks[current_sub_track_idx].title;
                osd_notification_time = now;
                std::cout << "\n[VMP Subtitles] Switched to track: " << available_sub_tracks[current_sub_track_idx].title << std::endl;
            } else {
                osd_notification = "Sub: No tracks";
                osd_notification_time = now;
            }
            last_user_activity = now;
            d_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_D) == GLFW_RELEASE) {
            d_pressed_prev = false;
        }

        // Audio track cycle (A key alone, without Shift)
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS && !a_pressed_prev && !shift_held) {
            std::vector<std::string> tracks = player.get_audio_track_list();
            if (!tracks.empty()) {
                int cur_track = player.get_current_audio_track();
                int next_track = (cur_track + 1) % tracks.size();
                if (player.select_audio_track(next_track)) {
                    std::string short_desc = tracks[next_track];
                    size_t brace_pos = short_desc.find(")");
                    if (brace_pos != std::string::npos) {
                        short_desc = short_desc.substr(0, brace_pos + 1);
                    }
                    osd_notification = "Audio: " + short_desc;
                    osd_notification_time = now;
                    std::cout << "\n[VMP Engine] " << osd_notification << std::endl;
                }
            } else {
                osd_notification = "Audio: No tracks";
                osd_notification_time = now;
            }
            last_user_activity = now;
            a_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_A) == GLFW_RELEASE) {
            a_pressed_prev = false;
        }

        // Next file in playlist (N key)
        if (glfwGetKey(window, GLFW_KEY_N) == GLFW_PRESS && !n_pressed_prev) {
            if (current_file_idx + 1 < playlist_files.size()) {
                current_file_idx++;
                std::cout << "\n[VMP Engine] Playlist: Next file requested." << std::endl;
                load_playlist_file_fn(current_file_idx);
            } else {
                osd_notification = "End of playlist";
                osd_notification_time = now;
            }
            last_user_activity = now;
            n_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_N) == GLFW_RELEASE) {
            n_pressed_prev = false;
        }

        // Previous file in playlist (B key)
        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS && !b_pressed_prev) {
            if (current_file_idx > 0) {
                current_file_idx--;
                std::cout << "\n[VMP Engine] Playlist: Previous file requested." << std::endl;
                load_playlist_file_fn(current_file_idx);
            } else {
                osd_notification = "Start of playlist";
                osd_notification_time = now;
            }
            last_user_activity = now;
            b_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_B) == GLFW_RELEASE) {
            b_pressed_prev = false;
        }

        // Mute / Unmute (M key)
        if (glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS && !m_pressed_prev) {
            player.toggle_mute();
            if (player.is_muted()) {
                osd_notification = "Mute: ON 🔇";
            } else {
                int vol_pct = static_cast<int>(std::round(player.get_volume() * 100));
                osd_notification = "Volume: " + std::to_string(vol_pct) + "% 🔊";
            }
            osd_notification_time = now;
            last_user_activity = now;
            m_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_M) == GLFW_RELEASE) {
            m_pressed_prev = false;
        }

        // Playback Speed Controls ([ / ] / Backslash / Backspace)
        static const float speed_presets[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
        if (glfwGetKey(window, GLFW_KEY_LEFT_BRACKET) == GLFW_PRESS && !left_bracket_prev) {
            float cur_spd = player.get_playback_speed();
            float new_spd = 0.5f;
            for (int i = 5; i >= 0; i--) {
                if (speed_presets[i] < cur_spd - 0.05f) {
                    new_spd = speed_presets[i];
                    break;
                }
            }
            player.set_playback_speed(new_spd);
            char spd_buf[32];
            snprintf(spd_buf, sizeof(spd_buf), "Speed: %.2fx", new_spd);
            osd_notification = std::string(spd_buf);
            osd_notification_time = now;
            last_user_activity = now;
            left_bracket_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_LEFT_BRACKET) == GLFW_RELEASE) {
            left_bracket_prev = false;
        }

        if (glfwGetKey(window, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS && !right_bracket_prev) {
            float cur_spd = player.get_playback_speed();
            float new_spd = 2.0f;
            for (int i = 0; i < 6; i++) {
                if (speed_presets[i] > cur_spd + 0.05f) {
                    new_spd = speed_presets[i];
                    break;
                }
            }
            player.set_playback_speed(new_spd);
            char spd_buf[32];
            snprintf(spd_buf, sizeof(spd_buf), "Speed: %.2fx", new_spd);
            osd_notification = std::string(spd_buf);
            osd_notification_time = now;
            last_user_activity = now;
            right_bracket_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_RIGHT_BRACKET) == GLFW_RELEASE) {
            right_bracket_prev = false;
        }

        if ((glfwGetKey(window, GLFW_KEY_BACKSLASH) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_BACKSPACE) == GLFW_PRESS) && !bs_prev) {
            player.set_playback_speed(1.0f);
            osd_notification = "Speed: 1.00x (Normal)";
            osd_notification_time = now;
            last_user_activity = now;
            bs_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_BACKSLASH) == GLFW_RELEASE && glfwGetKey(window, GLFW_KEY_BACKSPACE) == GLFW_RELEASE) {
            bs_prev = false;
        }

        // Screenshot capture (F12 key)
        if (glfwGetKey(window, GLFW_KEY_F12) == GLFW_PRESS && !f12_pressed_prev) {
            auto t_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm* tm_now = std::localtime(&t_now);
            char name_buf[128];
            std::strftime(name_buf, sizeof(name_buf), "vmp_screenshot_%Y%m%d_%H%M%S.bmp", tm_now);
            
            const char* home = std::getenv("HOME");
            std::string shot_dir = (home && std::filesystem::exists(std::string(home) + "/Pictures")) ? std::string(home) + "/Pictures" : ".";
            std::string shot_path = shot_dir + "/" + name_buf;

            if (ShaderRenderer::save_framebuffer_screenshot(width, height, shot_path)) {
                osd_notification = "Screenshot Saved! 📸";
                std::cout << "\n[VMP Engine] Screenshot saved to: " << shot_path << std::endl;
            } else {
                osd_notification = "Screenshot Failed!";
            }
            osd_notification_time = now;
            last_user_activity = now;
            f12_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_F12) == GLFW_RELEASE) {
            f12_pressed_prev = false;
        }

        // Subtitle Delay Offset (Z / X keys)
        if (glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS && !z_pressed_prev) {
            subtitle_delay_sec -= 0.1;
            int ms = static_cast<int>(std::round(subtitle_delay_sec * 1000.0));
            osd_notification = "Sub Delay: " + std::string(ms > 0 ? "+" : "") + std::to_string(ms) + " ms";
            osd_notification_time = now;
            last_user_activity = now;
            z_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_Z) == GLFW_RELEASE) {
            z_pressed_prev = false;
        }

        if (glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS && !x_pressed_prev) {
            subtitle_delay_sec += 0.1;
            int ms = static_cast<int>(std::round(subtitle_delay_sec * 1000.0));
            osd_notification = "Sub Delay: " + std::string(ms > 0 ? "+" : "") + std::to_string(ms) + " ms";
            osd_notification_time = now;
            last_user_activity = now;
            x_pressed_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_X) == GLFW_RELEASE) {
            x_pressed_prev = false;
        }

        // GPU Color Grading Controls (1-2: Brightness, 3-4: Contrast, 5-6: Saturation, 0: Reset)
        if (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS && !k1_prev) {
            renderer.set_brightness(renderer.get_brightness() - 0.05f);
            int val = static_cast<int>(std::round(renderer.get_brightness() * 100));
            osd_notification = "Brightness: " + std::string(val > 0 ? "+" : "") + std::to_string(val) + "%";
            osd_notification_time = now; last_user_activity = now; k1_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_1) == GLFW_RELEASE) k1_prev = false;

        if (glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS && !k2_prev) {
            renderer.set_brightness(renderer.get_brightness() + 0.05f);
            int val = static_cast<int>(std::round(renderer.get_brightness() * 100));
            osd_notification = "Brightness: " + std::string(val > 0 ? "+" : "") + std::to_string(val) + "%";
            osd_notification_time = now; last_user_activity = now; k2_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_2) == GLFW_RELEASE) k2_prev = false;

        if (glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS && !k3_prev) {
            renderer.set_contrast(renderer.get_contrast() - 0.1f);
            int val = static_cast<int>(std::round(renderer.get_contrast() * 100));
            osd_notification = "Contrast: " + std::to_string(val) + "%";
            osd_notification_time = now; last_user_activity = now; k3_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_3) == GLFW_RELEASE) k3_prev = false;

        if (glfwGetKey(window, GLFW_KEY_4) == GLFW_PRESS && !k4_prev) {
            renderer.set_contrast(renderer.get_contrast() + 0.1f);
            int val = static_cast<int>(std::round(renderer.get_contrast() * 100));
            osd_notification = "Contrast: " + std::to_string(val) + "%";
            osd_notification_time = now; last_user_activity = now; k4_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_4) == GLFW_RELEASE) k4_prev = false;

        if (glfwGetKey(window, GLFW_KEY_5) == GLFW_PRESS && !k5_prev) {
            renderer.set_saturation(renderer.get_saturation() - 0.1f);
            int val = static_cast<int>(std::round(renderer.get_saturation() * 100));
            osd_notification = "Saturation: " + std::to_string(val) + "%";
            osd_notification_time = now; last_user_activity = now; k5_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_5) == GLFW_RELEASE) k5_prev = false;

        if (glfwGetKey(window, GLFW_KEY_6) == GLFW_PRESS && !k6_prev) {
            renderer.set_saturation(renderer.get_saturation() + 0.1f);
            int val = static_cast<int>(std::round(renderer.get_saturation() * 100));
            osd_notification = "Saturation: " + std::to_string(val) + "%";
            osd_notification_time = now; last_user_activity = now; k6_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_6) == GLFW_RELEASE) k6_prev = false;

        if (glfwGetKey(window, GLFW_KEY_0) == GLFW_PRESS && !k0_prev) {
            renderer.reset_color_grading();
            osd_notification = "Color Grading: Reset";
            osd_notification_time = now; last_user_activity = now; k0_prev = true;
        } else if (glfwGetKey(window, GLFW_KEY_0) == GLFW_RELEASE) k0_prev = false;

        renderer.set_transform(current_zoom, pan_x, pan_y);

        // Check for video end (next file transition or loop)
        if (player.get_duration() > 0.0 && player.get_current_time() >= player.get_duration() - 0.25) {
            if (!eof_handled) {
                if (loop_mode) {
                    std::cout << "\n[VMP Engine] Looping: Seeking back to start." << std::endl;
                    player.seek_to_time(0.0);
                } else if (current_file_idx + 1 < playlist_files.size()) {
                    current_file_idx++;
                    std::cout << "\n[VMP Engine] Playlist: Auto-transition to next file." << std::endl;
                    if (!load_playlist_file_fn(current_file_idx)) {
                        std::cerr << "[VMP Error] Failed to auto-load next playlist file." << std::endl;
                    }
                } else {
                    // Explicitly pause and rewind to start
                    player.set_paused(true);
                    player.seek_to_time(0.0);
                    osd_notification = "Playlist: Ended";
                    osd_notification_time = now;
                }
                eof_handled = true;
            }
        } else {
            eof_handled = false;
        }

        auto t_loop_start = std::chrono::high_resolution_clock::now();
        player.render_current_frame(renderer, width, height, !is_fullscreen);
        auto t_render_done = std::chrono::high_resolution_clock::now();

        // Fetch active subtitle (with user delay offset)
        std::string active_sub = "";
        if (show_subtitles && !subtitle_entries.empty()) {
            active_sub = SubtitleParser::get_active_subtitle(subtitle_entries, player.get_current_time(), subtitle_delay_sec);
        }

        // Fetch active OSD notification
        std::string active_note = "";
        if (std::chrono::duration<double>(now - osd_notification_time).count() < 2.0) {
            active_note = osd_notification;
        }

        // Render OSD Control Bar Overlay
        VMPStats stats = player.get_stats();
        std::string video_res = std::to_string(stats.width) + "x" + std::to_string(stats.height);
        renderer.render_ui_overlay(width, height, player.get_current_time(), player.get_duration(), 
                                   player.is_paused(), (is_fullscreen || glfwGetWindowAttrib(window, GLFW_MAXIMIZED)), ui_alpha, mx, my, is_scrubbing, 
                                   show_stats, video_filename, video_size, video_date, 
                                   video_res, stats.codec_name, stats.fps, stats.hw_acceleration_status,
                                   active_sub, active_note);

        if (!is_fullscreen) {
            renderer.render_vlc_menu_bar(width, height, mx, my, active_menu_idx, 1.0f);
            if (active_menu_idx >= 0) {
                renderer.render_vlc_dropdown(width, height, mx, my, active_menu_idx);
            }
        }
        if (context_menu_active) {
            renderer.render_vlc_context_menu(width, height, mx, my, context_menu_x, context_menu_y, context_submenu_idx);
        }
        auto t_ui_done = std::chrono::high_resolution_clock::now();

        auto t_swap_start = std::chrono::high_resolution_clock::now();
        glfwSwapBuffers(window);
        auto t_swap_done = std::chrono::high_resolution_clock::now();
        glfwPollEvents();

        if (std::chrono::duration<double>(now - last_title_update).count() >= 0.2) {
            double elapsed_sec = std::chrono::duration<double>(now - start_time_global).count();
            
            telemetry.record_sample(elapsed_sec, stats.fps, stats.decode_time_ms);

            std::string status_str = player.is_paused() ? "PAUSED ❚❚" : "PLAYING ▶";
            std::string time_str = format_time(player.get_current_time(), has_hours) + " / " + format_time(player.get_duration(), has_hours);
            std::string title = video_filename.empty() ? "VMP" : (video_filename + " - VMP");
            glfwSetWindowTitle(window, title.c_str());

            double ms_render = std::chrono::duration<double, std::milli>(t_render_done - t_loop_start).count();
            double ms_ui = std::chrono::duration<double, std::milli>(t_ui_done - t_render_done).count();
            double ms_swap = std::chrono::duration<double, std::milli>(t_swap_done - t_swap_start).count();

            std::cout << "\r[VMP Status] " << status_str << " | Time: " << time_str
                      << " | FPS: " << std::setw(3) << static_cast<int>(stats.fps)
                      << " | Dec: " << std::fixed << std::setprecision(1) << stats.decode_time_ms << "ms"
                      << " | Rnd: " << ms_render << "ms"
                      << " | UI: " << ms_ui << "ms"
                      << " | Swp: " << ms_swap << "ms"
                      << " | Buf: " << stats.buffered_frames << std::flush;

            last_title_update = now;
        }

        bool esc_pressed_now = (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS);
        if (esc_pressed_now && !esc_pressed_prev) {
            if (context_menu_active) {
                context_menu_active = false;
                context_submenu_idx = -1;
            } else if (active_menu_idx >= 0) {
                active_menu_idx = -1;
            } else if (is_fullscreen) {
                toggle_fullscreen_fn(window);
            } else {
                if (!video_filename.empty() && current_file_idx < playlist_files.size()) {
                    ResumeManager::get_instance().save_position(playlist_files[current_file_idx], player.get_current_time(), player.get_duration());
                }
                player.close_file();
                renderer.clear_video_frame();
                thumb_gen.stop();
                video_filename.clear();
                glfwSetWindowTitle(window, "VMP");
                if (!folder_media_items.empty()) {
                    app_state = AppState::FOLDER_GALLERY;
                } else {
                    app_state = AppState::WELCOME;
                }
            }
            esc_pressed_prev = true;
        } else if (!esc_pressed_now) {
            esc_pressed_prev = false;
        }
    }

    std::cout << "\n\n[VMP Engine] Shutting down clean." << std::endl;
    thumb_gen.stop();
    player.stop();

    if (!playlist_files.empty() && current_file_idx < playlist_files.size()) {
        ResumeManager::get_instance().save_position(playlist_files[current_file_idx], player.get_current_time(), player.get_duration());
    }
    ResumeManager::get_instance().flush();

    if (!export_filepath.empty()) {
        VMPStats stats = player.get_stats();
        telemetry.export_json(export_filepath, stats.codec_name, stats.width, stats.height, stats.hw_acceleration_status);
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
