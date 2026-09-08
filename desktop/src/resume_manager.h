#ifndef VMP_RESUME_MANAGER_H
#define VMP_RESUME_MANAGER_H

#include <string>
#include <unordered_map>
#include <mutex>

struct PlaybackHistoryEntry {
    double last_position_sec = 0.0;
    double duration_sec = 0.0;
    long long timestamp_epoch = 0;
};

class ResumeManager {
public:
    static ResumeManager& get_instance();

    double get_saved_position(const std::string& filepath);
    void save_position(const std::string& filepath, double current_sec, double total_sec);
    void clear_position(const std::string& filepath);
    void clear_all();
    std::unordered_map<std::string, PlaybackHistoryEntry> get_all_entries();
    void flush();

private:
    ResumeManager();
    ~ResumeManager();

    std::string config_file_path;
    std::unordered_map<std::string, PlaybackHistoryEntry> history;
    std::mutex history_mutex;
    bool dirty = false;

    void load_from_disk();
    void save_to_disk();
    std::string get_storage_path();
};

#endif // VMP_RESUME_MANAGER_H
