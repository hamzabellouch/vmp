#ifndef VMP_THUMBNAIL_GENERATOR_H
#define VMP_THUMBNAIL_GENERATOR_H

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>

struct ThumbnailItem {
    double pts = 0.0;
    int width = 160;
    int height = 90;
    std::vector<uint8_t> rgb_data;
};

class ThumbnailGenerator {
public:
    ThumbnailGenerator();
    ~ThumbnailGenerator();

    void start_generation(const std::string& filepath, double duration_sec);
    void stop();

    bool get_thumbnail_at_time(double target_sec, std::vector<uint8_t>& out_rgb, int& out_w, int& out_h);
    bool has_thumbnails() const;

private:
    std::string current_filepath;
    double duration = 0.0;
    std::atomic<bool> is_running{false};
    std::thread worker_thread;

    mutable std::mutex thumb_mutex;
    std::vector<ThumbnailItem> cached_thumbnails;

    static constexpr int THUMB_W = 160;
    static constexpr int THUMB_H = 90;
    static constexpr int NUM_SLICES = 60; // 60 preview slices across the video

    void generate_loop();
};

#endif // VMP_THUMBNAIL_GENERATOR_H
