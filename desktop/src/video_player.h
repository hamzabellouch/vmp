#ifndef VMP_VIDEO_PLAYER_H
#define VMP_VIDEO_PLAYER_H

#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <string>

extern "C" {
#include <libswscale/swscale.h>
}

#include "hw_decoder.h"
#include "shader_renderer.h"
#include "audio_engine.h"

struct QueuedVideoFrame {
    AVFrame* frame = nullptr;
    double pts = 0.0;
    double decode_time_ms = 0.0;
};

struct VMPStats {
    double fps = 0.0;
    double stream_fps = 0.0;
    double decode_time_ms = 0.0;
    int width = 0;
    int height = 0;
    long total_frames_decoded = 0;
    int buffered_frames = 0;
    std::string codec_name;
    std::string hw_acceleration_status;
    bool has_audio = false;
};

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    bool open_file(const std::string& filepath);
    void close_file();
    void start();
    void stop();
    void toggle_pause();
    void set_paused(bool pause_state);
    void seek_relative(double seconds);
    void seek_to_time(double seconds);
    void seek_to_ratio(double ratio);
    
    double get_current_time() const { return current_pts_sec; }
    double get_duration() const { return duration_sec; }
    double get_stream_fps() const { return stream_fps; }
    
    void render_current_frame(ShaderRenderer& renderer, int win_w, int win_h);
    
    VMPStats get_stats();
    bool is_playing() const { return playing; }
    bool is_paused() const { return paused; }
    
    std::vector<std::string> get_audio_track_list();
    bool select_audio_track(int track_vector_idx);
    int get_current_audio_track() const { return current_audio_track_index; }

    void set_volume(float vol) { audio_engine.set_volume(vol); }
    float get_volume() const { return audio_engine.get_volume(); }
    void toggle_mute() { audio_engine.toggle_mute(); }
    bool is_muted() const { return audio_engine.is_audio_muted(); }

    void set_playback_speed(float speed);
    float get_playback_speed() const { return playback_speed; }

private:
    float playback_speed = 1.0f;
    double stream_fps = 24.0;
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* video_codec_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    
    int video_stream_index = -1;
    int audio_stream_index = -1;
    
    HardwareDecoder hw_decoder;
    AudioEngine audio_engine;
    
    std::atomic<bool> playing{false};
    std::atomic<bool> paused{false};
    std::thread decode_thread;
    
    AVFrame* current_frame = nullptr;
    AVFrame* sw_frame = nullptr;
    AVFrame* converted_frame = nullptr;
    AVFrame* audio_frame = nullptr;

    SwsContext* sws_ctx = nullptr;
    
    std::atomic<double> current_pts_sec{0.0};
    double duration_sec = 0.0;

    std::atomic<bool> seek_requested{false};
    std::atomic<double> seek_target_sec{0.0};
    std::atomic<bool> seeking_preroll{false};
    std::atomic<double> preroll_target_sec{0.0};
    
    std::mutex frame_mutex;
    std::queue<QueuedVideoFrame> frame_queue;
    static constexpr size_t MAX_FRAME_QUEUE_SIZE = 32;
    bool has_new_frame = false;
    
    VMPStats stats;
    std::chrono::high_resolution_clock::time_point last_render_fps_check;
    int rendered_frames_count = 0;
    double rendered_fps = 0.0;
    
    std::vector<int> audio_tracks;
    int current_audio_track_index = -1;

    // Video Clock Pacing for silent / video-only playback
    std::chrono::high_resolution_clock::time_point video_clock_start;
    double video_clock_base_pts = 0.0;
    bool video_clock_initialized = false;
    void reset_video_clock(double base_pts);

    void decode_loop();
    void flush_frame_queue();
};

#endif // VMP_VIDEO_PLAYER_H
