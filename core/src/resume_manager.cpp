#include "resume_manager.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <cstdlib>

ResumeManager& ResumeManager::get_instance() {
    static ResumeManager instance;
    return instance;
}

ResumeManager::ResumeManager() {
    config_file_path = get_storage_path();
    load_from_disk();
}

ResumeManager::~ResumeManager() {
    flush();
}

std::string ResumeManager::get_storage_path() {
    const char* home = std::getenv("HOME");
    if (home) {
        std::string config_dir = std::string(home) + "/.config";
        try {
            if (!std::filesystem::exists(config_dir)) {
                std::filesystem::create_directories(config_dir);
            }
            return config_dir + "/vmp_resume.json";
        } catch (...) {
            // fallback
        }
    }
    return ".vmp_resume.json";
}

void ResumeManager::load_from_disk() {
    std::lock_guard<std::mutex> lock(history_mutex);
    history.clear();
    dirty = false;

    if (!std::filesystem::exists(config_file_path)) {
        return;
    }

    std::ifstream file(config_file_path);
    if (!file.is_open()) return;

    std::string line;
    std::string current_file = "";
    PlaybackHistoryEntry entry;

    // Simple robust key-value parser for JSON
    while (std::getline(file, line)) {
        size_t quote1 = line.find("\"");
        if (quote1 == std::string::npos) continue;
        size_t quote2 = line.find("\"", quote1 + 1);
        if (quote2 == std::string::npos) continue;

        std::string key = line.substr(quote1 + 1, quote2 - quote1 - 1);
        size_t colon = line.find(":", quote2);

        if (colon != std::string::npos) {
            std::string val_str = line.substr(colon + 1);
            // Trim whitespace, commas, brackets
            size_t start = val_str.find_first_not_of(" \t\r\n{\",");
            size_t end = val_str.find_last_not_of(" \t\r\n}\",");
            if (start != std::string::npos && end != std::string::npos) {
                val_str = val_str.substr(start, end - start + 1);
            }

            if (key == "file") {
                current_file = val_str;
            } else if (key == "pos" && !current_file.empty()) {
                entry.last_position_sec = std::strtod(val_str.c_str(), nullptr);
            } else if (key == "duration" && !current_file.empty()) {
                entry.duration_sec = std::strtod(val_str.c_str(), nullptr);
            } else if (key == "epoch" && !current_file.empty()) {
                entry.timestamp_epoch = std::strtoll(val_str.c_str(), nullptr, 10);
                history[current_file] = entry;
                current_file = "";
                entry = PlaybackHistoryEntry{};
            }
        }
    }
}

void ResumeManager::save_to_disk() {
    if (!dirty) return;

    std::ofstream file(config_file_path, std::ios::trunc);
    if (!file.is_open()) return;

    file << "[\n";
    bool first = true;
    for (const auto& pair : history) {
        if (!first) file << ",\n";
        first = false;
        file << "  {\n";
        file << "    \"file\": \"" << pair.first << "\",\n";
        file << "    \"pos\": " << pair.second.last_position_sec << ",\n";
        file << "    \"duration\": " << pair.second.duration_sec << ",\n";
        file << "    \"epoch\": " << pair.second.timestamp_epoch << "\n";
        file << "  }";
    }
    file << "\n]\n";
    dirty = false;
}

double ResumeManager::get_saved_position(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(history_mutex);
    auto it = history.find(filepath);
    if (it != history.end()) {
        double pos = it->second.last_position_sec;
        double dur = it->second.duration_sec;
        // Only resume if watched more than 8 seconds and not within 15 seconds of the end
        if (pos > 8.0 && (dur <= 0.0 || pos < dur - 15.0)) {
            return pos;
        }
    }
    return 0.0;
}

void ResumeManager::save_position(const std::string& filepath, double current_sec, double total_sec) {
    std::lock_guard<std::mutex> lock(history_mutex);
    if (filepath.empty()) return;

    // If near the end of video (last 15 seconds), reset position to start from beginning next time
    if (total_sec > 0.0 && current_sec >= total_sec - 15.0) {
        current_sec = 0.0;
    }

    PlaybackHistoryEntry entry;
    entry.last_position_sec = current_sec;
    entry.duration_sec = total_sec;
    entry.timestamp_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    history[filepath] = entry;
    dirty = true;
}

void ResumeManager::clear_position(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(history_mutex);
    auto it = history.find(filepath);
    if (it != history.end()) {
        history.erase(it);
        dirty = true;
    }
}

void ResumeManager::clear_all() {
    std::lock_guard<std::mutex> lock(history_mutex);
    history.clear();
    dirty = true;
    save_to_disk();
}

std::unordered_map<std::string, PlaybackHistoryEntry> ResumeManager::get_all_entries() {
    std::lock_guard<std::mutex> lock(history_mutex);
    return history;
}

void ResumeManager::flush() {
    std::lock_guard<std::mutex> lock(history_mutex);
    save_to_disk();
}
