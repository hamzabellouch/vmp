#include "audio_engine.h"
#include <cstring>
#include <cmath>

AudioEngine::AudioEngine() {
    last_callback_time = std::chrono::high_resolution_clock::now();
}

AudioEngine::~AudioEngine() {
    close();
}

void AudioEngine::sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    AudioEngine* engine = static_cast<AudioEngine*>(userdata);
    std::lock_guard<std::mutex> lock(engine->audio_mutex);

    memset(stream, 0, len);
    if (engine->audio_queue.empty() || engine->is_muted) {
        engine->last_callback_time = std::chrono::high_resolution_clock::now();
        return;
    }

    size_t bytes_needed = static_cast<size_t>(len);
    size_t stream_pos = 0;
    int bytes_per_frame = engine->bytes_per_sample * engine->channels;

    while (bytes_needed > 0 && !engine->audio_queue.empty()) {
        AudioChunk& chunk = engine->audio_queue.front();

        // Calculate PTS of the exact sample being copied
        if (bytes_per_frame > 0 && engine->sample_rate > 0) {
            double sample_offset = static_cast<double>(chunk.read_offset / bytes_per_frame);
            double exact_pts = chunk.pts + (sample_offset / engine->sample_rate);
            engine->current_playing_pts = exact_pts;
            engine->clock_initialized = true;
        }

        size_t chunk_remaining = chunk.pcm_data.size() - chunk.read_offset;
        size_t to_copy = std::min(bytes_needed, chunk_remaining);

        int sdl_vol = static_cast<int>(SDL_MIX_MAXVOLUME * std::min(1.0f, engine->current_volume));
        SDL_MixAudioFormat(stream + stream_pos, chunk.pcm_data.data() + chunk.read_offset, 
                           AUDIO_S16SYS, to_copy, sdl_vol);

        chunk.read_offset += to_copy;
        stream_pos += to_copy;
        bytes_needed -= to_copy;

        if (engine->total_buffered_bytes >= to_copy) {
            engine->total_buffered_bytes -= to_copy;
        } else {
            engine->total_buffered_bytes = 0;
        }

        if (chunk.read_offset >= chunk.pcm_data.size()) {
            engine->audio_queue.pop_front();
        }
    }

    // Volume boost (> 100%) digital gain with soft clipping
    if (engine->current_volume > 1.0f) {
        int16_t* samples = reinterpret_cast<int16_t*>(stream);
        int num_samples = len / sizeof(int16_t);
        float gain = engine->current_volume;
        for (int i = 0; i < num_samples; i++) {
            int32_t val = static_cast<int32_t>(samples[i] * gain);
            samples[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, val)));
        }
    }

    engine->last_callback_time = std::chrono::high_resolution_clock::now();
}

bool AudioEngine::setup_swr(float speed) {
    if (swr_ctx) {
        swr_free(&swr_ctx);
        swr_ctx = nullptr;
    }
    if (!codec_info_saved) return false;

    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, channels);

    int in_rate = static_cast<int>(std::round(codec_sample_rate * speed));
    if (in_rate < 8000) in_rate = 8000;

    int ret = swr_alloc_set_opts2(&swr_ctx, &out_ch_layout, AV_SAMPLE_FMT_S16, sample_rate,
                                  &codec_ch_layout, codec_sample_fmt, in_rate, 0, NULL);
    av_channel_layout_uninit(&out_ch_layout);
    if (ret < 0 || !swr_ctx) return false;
    return (swr_init(swr_ctx) >= 0);
}

bool AudioEngine::init_audio(AVCodecContext* codec_ctx) {
    close();

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        std::cerr << "[VMP Audio] Failed to initialize SDL Audio: " << SDL_GetError() << std::endl;
        return false;
    }

    sample_rate = codec_ctx->sample_rate;
    channels = codec_ctx->ch_layout.nb_channels > 0 ? codec_ctx->ch_layout.nb_channels : 2;

    if (codec_info_saved) {
        av_channel_layout_uninit(&codec_ch_layout);
    }
    codec_sample_rate = codec_ctx->sample_rate;
    codec_sample_fmt = codec_ctx->sample_fmt;
    av_channel_layout_copy(&codec_ch_layout, &codec_ctx->ch_layout);
    codec_info_saved = true;

    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        audio_queue.clear();
        total_buffered_bytes = 0;
        clock_initialized = false;
        is_paused = false;
        current_playing_pts = 0.0;
        next_expected_pts = 0.0;
        last_callback_time = std::chrono::high_resolution_clock::now();
    }

    SDL_AudioSpec wanted_spec, obtained_spec;
    SDL_zero(wanted_spec);
    wanted_spec.freq = sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = channels;
    wanted_spec.samples = 1024; // Low-latency 1024 sample buffer (~21ms at 48kHz)
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = this;

    audio_device_id = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &obtained_spec, 0);
    if (audio_device_id == 0) {
        std::cerr << "[VMP Audio] Failed to open audio device: " << SDL_GetError() << std::endl;
        return false;
    }

    sdl_buffer_samples = (obtained_spec.samples > 0) ? obtained_spec.samples : 1024;

    if (!setup_swr(playback_speed)) {
        std::cerr << "[VMP Audio] Failed to setup SwrContext resampler." << std::endl;
        return false;
    }

    SDL_PauseAudioDevice(audio_device_id, 0); // Start audio playback stream
    std::cout << "[VMP Audio] Precision Timestamped Audio Engine Active: " 
              << sample_rate << "Hz | " << channels << " Channels | Latency: " 
              << (sdl_buffer_samples * 1000 / sample_rate) << "ms." << std::endl;
    return true;
}

void AudioEngine::play_chunk(AVFrame* frame, double pts_sec) {
    if (!swr_ctx || audio_device_id == 0) return;

    int out_samples = swr_get_out_samples(swr_ctx, frame->nb_samples);
    int required_bytes = av_samples_get_buffer_size(NULL, channels, out_samples, AV_SAMPLE_FMT_S16, 0);
    if (required_bytes <= 0) return;

    std::vector<uint8_t> converted_data(required_bytes);
    uint8_t* out_buffer = converted_data.data();
    int converted = swr_convert(swr_ctx, &out_buffer, out_samples, 
                                (const uint8_t**)frame->data, frame->nb_samples);

    if (converted > 0) {
        int buffer_size = av_samples_get_buffer_size(NULL, channels, converted, AV_SAMPLE_FMT_S16, 0);
        if (buffer_size <= 0) return;
        converted_data.resize(buffer_size);

        std::lock_guard<std::mutex> lock(audio_mutex);

        double actual_pts = pts_sec;
        if (actual_pts < 0.0) {
            actual_pts = next_expected_pts;
        }

        double duration_sec = static_cast<double>(converted) / sample_rate;
        next_expected_pts = actual_pts + duration_sec;

        if (total_buffered_bytes + buffer_size > MAX_BUFFERED_BYTES * 2) {
            // Prevent unbounded queue growth if audio device is stalled
            return;
        }

        AudioChunk chunk;
        chunk.pcm_data = std::move(converted_data);
        chunk.read_offset = 0;
        chunk.pts = actual_pts;

        audio_queue.push_back(std::move(chunk));
        total_buffered_bytes += buffer_size;

        if (!clock_initialized) {
            current_playing_pts = actual_pts;
            last_callback_time = std::chrono::high_resolution_clock::now();
            clock_initialized = true;
        }
    }
}

void AudioEngine::set_volume(float volume) {
    std::lock_guard<std::mutex> lock(audio_mutex);
    current_volume = std::max(0.0f, std::min(1.5f, volume));
    if (current_volume > 0.0f) {
        is_muted = false;
    }
}

void AudioEngine::toggle_mute() {
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (is_muted) {
        is_muted = false;
        current_volume = (pre_mute_volume > 0.05f) ? pre_mute_volume : 1.0f;
    } else {
        pre_mute_volume = current_volume;
        is_muted = true;
    }
}

void AudioEngine::set_playback_speed(float speed) {
    std::lock_guard<std::mutex> lock(audio_mutex);
    float clamped = std::max(0.25f, std::min(4.0f, speed));
    if (std::abs(playback_speed - clamped) > 0.01f) {
        playback_speed = clamped;
        setup_swr(playback_speed);
    }
}

void AudioEngine::set_audio_clock(double pts_sec) {
    std::lock_guard<std::mutex> lock(audio_mutex);
    audio_queue.clear();
    total_buffered_bytes = 0;
    current_playing_pts = std::max(0.0, pts_sec);
    next_expected_pts = current_playing_pts;
    last_callback_time = std::chrono::high_resolution_clock::now();
    clock_initialized = true;
}

void AudioEngine::set_paused(bool pause_state) {
    std::lock_guard<std::mutex> lock(audio_mutex);
    is_paused = pause_state;
    if (audio_device_id != 0) {
        SDL_PauseAudioDevice(audio_device_id, is_paused ? 1 : 0);
    }
    if (!is_paused) {
        last_callback_time = std::chrono::high_resolution_clock::now();
    }
}

void AudioEngine::flush() {
    std::lock_guard<std::mutex> lock(audio_mutex);
    audio_queue.clear();
    total_buffered_bytes = 0;
    clock_initialized = false;
    current_playing_pts = 0.0;
    next_expected_pts = 0.0;
}

double AudioEngine::get_audio_clock() {
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (!clock_initialized) {
        return 0.0;
    }

    if (is_paused) {
        return current_playing_pts;
    }

    auto now = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_callback_time).count() * playback_speed;
    double hw_delay = (sample_rate > 0) ? (static_cast<double>(sdl_buffer_samples) / sample_rate) : 0.0;

    double clk = current_playing_pts + elapsed - hw_delay;
    return std::max(0.0, clk);
}

size_t AudioEngine::get_buffer_size() {
    std::lock_guard<std::mutex> lock(audio_mutex);
    return total_buffered_bytes;
}

void AudioEngine::close() {
    if (audio_device_id != 0) {
        SDL_CloseAudioDevice(audio_device_id);
        audio_device_id = 0;
    }
    if (swr_ctx) {
        swr_free(&swr_ctx);
        swr_ctx = nullptr;
    }
    if (codec_info_saved) {
        av_channel_layout_uninit(&codec_ch_layout);
        codec_info_saved = false;
    }
    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        audio_queue.clear();
        total_buffered_bytes = 0;
    }
}
