#ifndef VMP_AUDIO_ENGINE_H
#define VMP_AUDIO_ENGINE_H

#include <iostream>
#include <vector>
#include <deque>
#include <mutex>
#include <chrono>
#include <SDL2/SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

struct AudioChunk {
    std::vector<uint8_t> pcm_data;
    size_t read_offset = 0;
    double pts = 0.0;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool init_audio(AVCodecContext* codec_ctx);
    void play_chunk(AVFrame* frame, double pts_sec = -1.0);
    void set_volume(float volume); // 0.0 to 1.5 (with 150% boost)
    float get_volume() const { return current_volume; }
    void toggle_mute();
    bool is_audio_muted() const { return is_muted; }
    void set_playback_speed(float speed);
    float get_playback_speed() const { return playback_speed; }
    void set_audio_clock(double pts_sec);
    void set_paused(bool pause_state);
    bool is_audio_paused() const { return is_paused; }
    void flush();
    void close();

    double get_audio_clock();
    size_t get_buffer_size();

private:
    SDL_AudioDeviceID audio_device_id = 0;
    SwrContext* swr_ctx = nullptr;
    
    int sample_rate = 44100;
    int channels = 2;
    int bytes_per_sample = 2; // AUDIO_S16SYS = 2 bytes
    float current_volume = 1.0f;
    float pre_mute_volume = 1.0f;
    bool is_muted = false;
    bool is_paused = false;
    float playback_speed = 1.0f;

    double current_playing_pts = 0.0;
    std::chrono::high_resolution_clock::time_point last_callback_time;
    bool clock_initialized = false;
    double next_expected_pts = 0.0;
    int sdl_buffer_samples = 1024;

    // Codec parameters for speed-adjusted resampling
    int codec_sample_rate = 44100;
    AVSampleFormat codec_sample_fmt = AV_SAMPLE_FMT_NONE;
    AVChannelLayout codec_ch_layout{};
    bool codec_info_saved = false;
    bool setup_swr(float speed);

    static void sdl_audio_callback(void* userdata, Uint8* stream, int len);
    std::mutex audio_mutex;
    
    std::deque<AudioChunk> audio_queue;
    size_t total_buffered_bytes = 0;
    static constexpr size_t MAX_BUFFERED_BYTES = 256 * 1024;
};

#endif // VMP_AUDIO_ENGINE_H

