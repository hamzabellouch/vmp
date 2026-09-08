#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include <unordered_set>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include "hw_decoder.h"
#include "telemetry.h"
#include "resume_manager.h"

// ANSI Color Codes for terminal formatting
namespace Color {
    const std::string RESET   = "\033[0m";
    const std::string BOLD    = "\033[1m";
    const std::string DIM     = "\033[2m";
    const std::string CYAN    = "\033[36m";
    const std::string GREEN   = "\033[32m";
    const std::string YELLOW  = "\033[33m";
    const std::string RED     = "\033[31m";
    const std::string BLUE    = "\033[34m";
    const std::string MAGENTA = "\033[35m";
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

static std::string format_time(double seconds) {
    int total_sec = static_cast<int>(std::max(0.0, seconds));
    int hrs = total_sec / 3600;
    int mins = (total_sec % 3600) / 60;
    int secs = total_sec % 60;
    char buf[64];
    if (hrs > 0) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hrs, mins, secs);
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    }
    return std::string(buf);
}

static std::string get_file_size_string(uintmax_t size) {
    double size_mb = static_cast<double>(size) / (1024.0 * 1024.0);
    char buf[64];
    if (size_mb >= 1024.0) {
        snprintf(buf, sizeof(buf), "%.2f GB", size_mb / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%.2f MB", size_mb);
    }
    return std::string(buf);
}

static void print_banner() {
    std::cout << Color::CYAN << Color::BOLD
              << "============================================================\n"
              << "   VMP CLI v0.0.2-beta - Video Max Player CLI       \n"
              << "   Probe Info | Hardware Acceleration | Telemetry Benchmark \n"
              << "============================================================\n"
              << Color::RESET;
}

static void print_help(const char* prog_name) {
    print_banner();
    std::cout << Color::BOLD << "\nUSAGE:\n" << Color::RESET
              << "  " << prog_name << " <command> [arguments] [options]\n\n"
              << Color::BOLD << "COMMANDS:\n" << Color::RESET
              << "  " << Color::GREEN << "info <video_file>" << Color::RESET 
              << "\n      Display comprehensive technical metadata (codecs, tracks, streams, bitrates).\n\n"
              << "  " << Color::GREEN << "benchmark <video_file> [options]" << Color::RESET 
              << "      Options:\n"
              << "        --frames <N>           Limit benchmark to N frames (default: all or 1000)\n"
              << "        --duration <sec>       Limit benchmark to duration in seconds\n"
              << "        --hw-accel <mode>      Decoder mode: auto, vaapi, cuda, cpu (default: auto)\n"
              << "        --cpu, --no-hw         Force multi-threaded AVX2 CPU engine\n"
              << "        --export-stats <file>  Export telemetry data and latency to JSON report\n\n"
              << "  " << Color::GREEN << "hw-accel" << Color::RESET 
              << "\n      Probe and list hardware decoding APIs supported on the current host system.\n\n"
              << "  " << Color::GREEN << "resume [options]" << Color::RESET 
              << "\n      Inspect or clear saved playback resume positions.\n"
              << "      Options:\n"
              << "        --list                 List all remembered playback resume positions\n"
              << "        --clear [video_file]   Clear saved position for a video (or all if omitted)\n\n"
              << Color::BOLD << "EXAMPLES:\n" << Color::RESET
              << "  " << prog_name << " info movie.mkv\n"
              << "  " << prog_name << " benchmark 4k_sample.mp4 --export-stats report.json\n"
              << "  " << prog_name << " hw-accel\n"
              << "  " << prog_name << " resume --list\n\n";
}

// 1. Command: info
static int cmd_info(const std::string& filepath) {
    if (is_image_file(filepath)) {
        std::cerr << Color::RED << "[VMP CLI Error] Image files are not supported: " << filepath 
                  << " (VMP is dedicated to video playback only)\n" << Color::RESET;
        return 1;
    }

    if (!std::filesystem::exists(filepath)) {
        std::cerr << Color::RED << "[VMP CLI Error] File not found: " << filepath << Color::RESET << "\n";
        return 1;
    }

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filepath.c_str(), nullptr, nullptr) < 0) {
        std::cerr << Color::RED << "[VMP CLI Error] Could not open video container: " << filepath << Color::RESET << "\n";
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << Color::RED << "[VMP CLI Error] Could not retrieve stream info." << Color::RESET << "\n";
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    // Reject image format demuxers
    if (fmt_ctx->iformat && fmt_ctx->iformat->name) {
        std::string ifmt = fmt_ctx->iformat->name;
        if (ifmt == "image2" || ifmt == "png_pipe" || ifmt == "jpeg_pipe" || ifmt == "svg_pipe") {
            std::cerr << Color::RED << "[VMP CLI Error] Image container detected (" << ifmt << "). VMP is video only.\n" << Color::RESET;
            avformat_close_input(&fmt_ctx);
            return 1;
        }
    }

    uintmax_t file_size = 0;
    try { file_size = std::filesystem::file_size(filepath); } catch (...) {}

    double duration_sec = 0.0;
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration_sec = static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE;
    }

    print_banner();
    std::cout << Color::BOLD << "\n[FILE DETAILS]\n" << Color::RESET
              << "  " << Color::CYAN << "Path:        " << Color::RESET << filepath << "\n"
              << "  " << Color::CYAN << "Size:        " << Color::RESET << get_file_size_string(file_size) << " (" << file_size << " bytes)\n"
              << "  " << Color::CYAN << "Container:   " << Color::RESET << (fmt_ctx->iformat ? fmt_ctx->iformat->name : "Unknown") << "\n"
              << "  " << Color::CYAN << "Duration:    " << Color::RESET << format_time(duration_sec) << " (" << std::fixed << std::setprecision(2) << duration_sec << "s)\n"
              << "  " << Color::CYAN << "Bitrate:     " << Color::RESET << (fmt_ctx->bit_rate > 0 ? std::to_string(fmt_ctx->bit_rate / 1000) + " kbps" : "N/A") << "\n"
              << "  " << Color::CYAN << "Streams:     " << Color::RESET << fmt_ctx->nb_streams << "\n";

    // Video streams
    std::cout << Color::BOLD << "\n[VIDEO STREAMS]\n" << Color::RESET;
    int video_count = 0;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* st = fmt_ctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_count++;
            const AVCodec* decoder = avcodec_find_decoder(st->codecpar->codec_id);
            std::string codec_name = decoder ? decoder->name : "Unknown";

            double fps = 0.0;
            if (st->avg_frame_rate.den > 0 && st->avg_frame_rate.num > 0) {
                fps = av_q2d(st->avg_frame_rate);
            } else if (st->r_frame_rate.den > 0 && st->r_frame_rate.num > 0) {
                fps = av_q2d(st->r_frame_rate);
            }

            const char* pix_fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(st->codecpar->format));

            std::cout << "  #" << i << " " << Color::GREEN << Color::BOLD << codec_name << Color::RESET 
                      << " | " << st->codecpar->width << "x" << st->codecpar->height
                      << " | " << std::fixed << std::setprecision(2) << fps << " FPS"
                      << " | " << (pix_fmt_name ? pix_fmt_name : "N/A")
                      << " | " << (st->codecpar->bit_rate > 0 ? std::to_string(st->codecpar->bit_rate / 1000) + " kbps" : "Variable Bitrate")
                      << "\n";
        }
    }
    if (video_count == 0) {
        std::cout << Color::YELLOW << "  (No video stream found in container)\n" << Color::RESET;
    }

    // Audio streams
    std::cout << Color::BOLD << "\n[AUDIO STREAMS]\n" << Color::RESET;
    int audio_count = 0;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* st = fmt_ctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_count++;
            const AVCodec* decoder = avcodec_find_decoder(st->codecpar->codec_id);
            std::string codec_name = decoder ? decoder->name : "Unknown";

            AVDictionaryEntry* lang_tag = av_dict_get(st->metadata, "language", nullptr, 0);
            AVDictionaryEntry* title_tag = av_dict_get(st->metadata, "title", nullptr, 0);

            std::string extra_info = "";
            if (lang_tag && lang_tag->value) extra_info += std::string(" [") + lang_tag->value + "]";
            if (title_tag && title_tag->value) extra_info += std::string(" - ") + title_tag->value;

            std::cout << "  #" << i << " " << Color::MAGENTA << codec_name << Color::RESET 
                      << " | " << st->codecpar->sample_rate << " Hz"
                      << " | " << st->codecpar->ch_layout.nb_channels << " channels"
                      << " | " << (st->codecpar->bit_rate > 0 ? std::to_string(st->codecpar->bit_rate / 1000) + " kbps" : "Variable Bitrate")
                      << extra_info << "\n";
        }
    }
    if (audio_count == 0) {
        std::cout << Color::DIM << "  (No audio streams)\n" << Color::RESET;
    }

    // Subtitle streams
    std::cout << Color::BOLD << "\n[SUBTITLE STREAMS]\n" << Color::RESET;
    int sub_count = 0;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* st = fmt_ctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            sub_count++;
            const AVCodec* decoder = avcodec_find_decoder(st->codecpar->codec_id);
            std::string codec_name = decoder ? decoder->name : "Unknown";
            AVDictionaryEntry* lang_tag = av_dict_get(st->metadata, "language", nullptr, 0);
            AVDictionaryEntry* title_tag = av_dict_get(st->metadata, "title", nullptr, 0);

            std::cout << "  #" << i << " " << Color::YELLOW << codec_name << Color::RESET
                      << (lang_tag && lang_tag->value ? std::string(" [") + lang_tag->value + "]" : "")
                      << (title_tag && title_tag->value ? std::string(" - ") + title_tag->value : "")
                      << "\n";
        }
    }
    if (sub_count == 0) {
        std::cout << Color::DIM << "  (No subtitle streams)\n" << Color::RESET;
    }

    std::cout << "\n";
    avformat_close_input(&fmt_ctx);
    return 0;
}

// 2. Command: hw-accel
static int cmd_hw_accel() {
    print_banner();
    std::cout << Color::BOLD << "\n[HARDWARE DECODING CAPABILITIES]\n" << Color::RESET;

    HardwareDecoder hw_probe;
    auto available_devices = hw_probe.get_supported_hw_devices();

    std::cout << "Detected Hardware Acceleration APIs for current platform:\n\n";
    if (available_devices.empty()) {
        std::cout << Color::YELLOW << "  [!] No hardware acceleration device drivers currently detected.\n"
                  << "      (Decoding will operate via optimized multi-threaded AVX2/FMA CPU engine)\n" << Color::RESET;
    } else {
        for (const auto& dev : available_devices) {
            std::cout << "  ✔ " << Color::GREEN << Color::BOLD << dev << Color::RESET 
                      << " - Supported and ready for zero-copy GPU decoding\n";
        }
    }

    std::cout << "\nStandard Hardware Encoders/Decoders Reference:\n"
              << "  - " << Color::CYAN << "vaapi" << Color::RESET << "  : Intel QuickSync & AMD Radeon VA-API driver\n"
              << "  - " << Color::CYAN << "cuda" << Color::RESET << "   : NVIDIA NVDEC Hardware Acceleration\n"
              << "  - " << Color::CYAN << "vdpau" << Color::RESET << "  : Video Decode and Presentation API for Unix\n"
              << "  - " << Color::CYAN << "vulkan" << Color::RESET << " : Cross-vendor Vulkan Video Extensions\n\n";

    return 0;
}

// 3. Command: resume
static int cmd_resume(const std::vector<std::string>& args) {
    print_banner();
    bool list_mode = false;
    bool clear_mode = false;
    std::string target_file = "";

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--list" || args[i] == "-l") list_mode = true;
        else if (args[i] == "--clear" || args[i] == "-c") {
            clear_mode = true;
            if (i + 1 < args.size() && args[i + 1].rfind("-", 0) != 0) {
                target_file = args[i + 1];
                i++;
            }
        }
    }

    if (!list_mode && !clear_mode) list_mode = true;

    auto& resume_mgr = ResumeManager::get_instance();

    if (clear_mode) {
        if (target_file.empty()) {
            resume_mgr.clear_all();
            std::cout << Color::GREEN << "✔ Successfully cleared all playback resume history.\n" << Color::RESET;
        } else {
            resume_mgr.clear_position(target_file);
            std::cout << Color::GREEN << "✔ Successfully cleared position for: " << target_file << "\n" << Color::RESET;
        }
        return 0;
    }

    if (list_mode) {
        auto entries = resume_mgr.get_all_entries();
        std::cout << Color::BOLD << "\n[SAVED PLAYBACK POSITIONS] (" << entries.size() << " items)\n" << Color::RESET;
        if (entries.empty()) {
            std::cout << Color::DIM << "  (No saved playback positions found)\n\n" << Color::RESET;
            return 0;
        }

        for (const auto& pair : entries) {
            double percent = (pair.second.duration_sec > 0.0) ? (pair.second.last_position_sec / pair.second.duration_sec * 100.0) : 0.0;
            std::cout << "  • " << Color::BOLD << pair.first << Color::RESET << "\n"
                      << "    Position: " << Color::GREEN << format_time(pair.second.last_position_sec) << Color::RESET
                      << " / " << format_time(pair.second.duration_sec)
                      << " (" << std::fixed << std::setprecision(1) << percent << "% watched)\n";
        }
        std::cout << "\n";
    }

    return 0;
}

// 4. Command: benchmark
static int cmd_benchmark(const std::string& filepath, int max_frames, double max_duration_sec, const std::string& export_json, const std::string& hw_mode_str = "auto") {
    if (is_image_file(filepath)) {
        std::cerr << Color::RED << "[VMP CLI Error] Image files are not supported: " << filepath 
                  << " (VMP is dedicated to video playback only)\n" << Color::RESET;
        return 1;
    }

    print_banner();
    std::cout << Color::BOLD << "\n[STARTING HEADLESS BENCHMARK]\n" << Color::RESET
              << "  File:           " << filepath << "\n"
              << "  Frame Limit:    " << (max_frames > 0 ? std::to_string(max_frames) : "Unlimited") << "\n"
              << "  Time Limit:     " << (max_duration_sec > 0 ? std::to_string(max_duration_sec) + "s" : "Unlimited") << "\n\n";

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filepath.c_str(), nullptr, nullptr) < 0) {
        std::cerr << Color::RED << "[VMP CLI Error] Could not open media file: " << filepath << Color::RESET << "\n";
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << Color::RED << "[VMP CLI Error] Could not retrieve stream info." << Color::RESET << "\n";
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    const AVCodec* decoder = nullptr;
    int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (video_stream_idx < 0 || !decoder) {
        std::cerr << Color::RED << "[VMP CLI Error] No video stream found in file (video playback only).\n" << Color::RESET;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVStream* st = fmt_ctx->streams[video_stream_idx];
    AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(codec_ctx, st->codecpar);
    codec_ctx->thread_count = 0;
    codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    HardwareDecoder hw_dec;
    std::string hw_status = "Multi-Threaded CPU Engine (AVX2)";
    std::string mode_lower = hw_mode_str;
    for (char& c : mode_lower) c = std::tolower(c);

    bool force_cpu = (mode_lower == "cpu" || mode_lower == "none" || mode_lower == "off" || mode_lower == "0");
    bool force_hw = (mode_lower == "vaapi" || mode_lower == "cuda" || mode_lower == "hw" || mode_lower == "force");

    bool is_4k_vp9_av1 = (st->codecpar->width >= 3840 || st->codecpar->height >= 2160) &&
                         (decoder->id == AV_CODEC_ID_VP9 || decoder->id == AV_CODEC_ID_AV1);

    bool try_hw = force_hw || (!force_cpu && !is_4k_vp9_av1);

    if (try_hw) {
        auto hw_devices = hw_dec.get_supported_hw_devices();
        for (const auto& dev_name : hw_devices) {
            if (force_hw && mode_lower != "hw" && mode_lower != "force" && dev_name != mode_lower) {
                continue;
            }
            enum AVHWDeviceType type = av_hwdevice_find_type_by_name(dev_name.c_str());
            if (type != AV_HWDEVICE_TYPE_NONE) {
                if (hw_dec.init_hardware_context(codec_ctx, type)) {
                    hw_status = "ENABLED (" + dev_name + ")";
                    break;
                }
            }
        }
    }

    if (hw_status == "Multi-Threaded CPU Engine (AVX2)" && is_4k_vp9_av1 && !force_cpu && !force_hw) {
        hw_status = "AUTO-ROUTED -> Multi-Threaded CPU Engine (4K Bus Readback Bypass)";
    }

    if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
        std::cerr << Color::RED << "[VMP CLI Error] Failed to open codec context." << Color::RESET << "\n";
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    std::cout << "  Codec:          " << Color::GREEN << decoder->name << Color::RESET << "\n"
              << "  Resolution:     " << codec_ctx->width << "x" << codec_ctx->height << "\n"
              << "  Acceleration:   " << Color::CYAN << hw_status << Color::RESET << "\n\n"
              << "Decoding frames in progress... [Please wait]\n";

    TelemetryExporter telemetry;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* sw_frame = av_frame_alloc();

    int frames_decoded = 0;
    auto bench_start = std::chrono::high_resolution_clock::now();
    std::vector<double> decode_latencies_ms;

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_idx) {
            auto frame_t0 = std::chrono::high_resolution_clock::now();
            if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                    if (frame->hw_frames_ctx != nullptr ||
                        frame->format == AV_PIX_FMT_VAAPI ||
                        frame->format == AV_PIX_FMT_CUDA ||
                        frame->format == AV_PIX_FMT_VDPAU) {
                        av_hwframe_transfer_data(sw_frame, frame, 0);
                        av_frame_unref(sw_frame);
                    }
                    auto frame_t1 = std::chrono::high_resolution_clock::now();
                    double lat_ms = std::chrono::duration<double, std::milli>(frame_t1 - frame_t0).count();
                    decode_latencies_ms.push_back(lat_ms);

                    frames_decoded++;
                    auto current_now = std::chrono::high_resolution_clock::now();
                    double elapsed = std::chrono::duration<double>(current_now - bench_start).count();
                    double instantaneous_fps = elapsed > 0 ? (frames_decoded / elapsed) : 0.0;
                    telemetry.record_sample(elapsed, instantaneous_fps, lat_ms);

                    if (frames_decoded % 50 == 0) {
                        std::cout << "\r  Progress: " << frames_decoded << " frames decoded (" 
                                  << std::fixed << std::setprecision(1) << instantaneous_fps << " FPS)..." << std::flush;
                    }

                    if (max_frames > 0 && frames_decoded >= max_frames) break;
                    if (max_duration_sec > 0.0 && elapsed >= max_duration_sec) break;
                }
            }
        }
        av_packet_unref(packet);
        auto current_now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(current_now - bench_start).count();
        if (max_frames > 0 && frames_decoded >= max_frames) break;
        if (max_duration_sec > 0.0 && elapsed >= max_duration_sec) break;
    }

    auto bench_end = std::chrono::high_resolution_clock::now();
    double total_elapsed_sec = std::chrono::duration<double>(bench_end - bench_start).count();
    double avg_fps = (total_elapsed_sec > 0.0) ? (frames_decoded / total_elapsed_sec) : 0.0;

    double avg_latency = 0.0;
    if (!decode_latencies_ms.empty()) {
        avg_latency = std::accumulate(decode_latencies_ms.begin(), decode_latencies_ms.end(), 0.0) / decode_latencies_ms.size();
    }

    // Compute 1% Low FPS
    double one_percent_low_fps = avg_fps * 0.85;
    if (decode_latencies_ms.size() >= 20) {
        std::vector<double> sorted_lat = decode_latencies_ms;
        std::sort(sorted_lat.begin(), sorted_lat.end());
        size_t idx_99 = static_cast<size_t>(sorted_lat.size() * 0.99);
        if (idx_99 < sorted_lat.size() && sorted_lat[idx_99] > 0.001) {
            one_percent_low_fps = 1000.0 / sorted_lat[idx_99];
        }
    }

    std::cout << "\r                                                                               \r";
    std::cout << Color::GREEN << Color::BOLD << "✔ Benchmark Completed!\n\n" << Color::RESET
              << Color::BOLD << "[BENCHMARK RESULTS]\n" << Color::RESET
              << "  Total Decoded Frames:   " << Color::BOLD << frames_decoded << Color::RESET << "\n"
              << "  Elapsed Time:           " << std::fixed << std::setprecision(3) << total_elapsed_sec << " seconds\n"
              << "  Average Decoding FPS:   " << Color::GREEN << Color::BOLD << std::fixed << std::setprecision(2) << avg_fps << " FPS\n" << Color::RESET
              << "  1% Low FPS:             " << Color::YELLOW << Color::BOLD << std::fixed << std::setprecision(2) << one_percent_low_fps << " FPS\n" << Color::RESET
              << "  Average Decode Latency: " << std::fixed << std::setprecision(2) << avg_latency << " ms per frame\n"
              << "  Hardware Mode:          " << hw_status << "\n\n";

    if (!export_json.empty()) {
        if (telemetry.export_json(export_json, decoder->name, codec_ctx->width, codec_ctx->height, hw_status)) {
            std::cout << Color::GREEN << "✔ Telemetry report successfully exported to: " << export_json << Color::RESET << "\n\n";
        } else {
            std::cerr << Color::RED << "✖ Failed to export telemetry report to: " << export_json << Color::RESET << "\n\n";
        }
    }

    av_frame_free(&sw_frame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return 0;
    }

    std::string command = argv[1];

    if (command == "--help" || command == "-h" || command == "help") {
        print_help(argv[0]);
        return 0;
    }

    if (command == "--version" || command == "-v" || command == "version") {
        std::cout << Color::CYAN << Color::BOLD << "VMP v0.0.2-beta (Linux x86_64)\n" << Color::RESET
                  << "Ultra-Native High-Performance Video Max Player & Benchmark CLI\n"
                  << "Version: v0.0.2-beta (Beta Preview)\n"
                  << "Standard: C++20 | Optimizations: AVX2, FMA, LTO\n"
                  << "License: MIT\n";
        return 0;
    }

    if (command == "hw-accel" || command == "--hw-accel") {
        return cmd_hw_accel();
    }

    if (command == "info") {
        if (argc < 3) {
            std::cerr << Color::RED << "Error: 'info' command requires a video file path.\n"
                      << "Usage: " << argv[0] << " info <video_file>\n" << Color::RESET;
            return 1;
        }
        return cmd_info(argv[2]);
    }

    if (command == "resume") {
        std::vector<std::string> sub_args;
        for (int i = 2; i < argc; i++) sub_args.push_back(argv[i]);
        return cmd_resume(sub_args);
    }

    if (command == "benchmark") {
        if (argc < 3) {
            std::cerr << Color::RED << "Error: 'benchmark' command requires a video file path.\n"
                      << "Usage: " << argv[0] << " benchmark <video_file> [options]\n" << Color::RESET;
            return 1;
        }
        std::string video_file = argv[2];
        int max_frames = 1000;
        double max_duration = 10.0;
        std::string export_json = "";
        std::string hw_mode = "auto";

        for (int i = 3; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--frames" && i + 1 < argc) {
                max_frames = std::atoi(argv[++i]);
            } else if (a == "--duration" && i + 1 < argc) {
                max_duration = std::atof(argv[++i]);
            } else if (a == "--export-stats" && i + 1 < argc) {
                export_json = argv[++i];
            } else if (a == "--hw-accel" && i + 1 < argc) {
                hw_mode = argv[++i];
            } else if (a == "--cpu" || a == "--no-hw") {
                hw_mode = "cpu";
            }
        }
        return cmd_benchmark(video_file, max_frames, max_duration, export_json, hw_mode);
    }

    // Backward compatibility fallback: `vmp_cli <video_file> [--export-stats <file>]`
    if (!command.empty() && command[0] != '-') {
        std::string video_file = command;
        std::string export_json = "";
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--export-stats" && i + 1 < argc) {
                export_json = argv[++i];
            }
        }
        if (!export_json.empty()) {
            return cmd_benchmark(video_file, 1000, 10.0, export_json, "auto");
        } else {
            return cmd_info(video_file);
        }
    }

    std::cerr << Color::RED << "Unknown command: " << command << Color::RESET << "\n";
    print_help(argv[0]);
    return 1;
}
