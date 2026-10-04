#include "config_manager.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <algorithm>

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

ConfigManager& ConfigManager::get_instance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager() {
    config_path = determine_default_config_path();
    load();
}

ConfigManager::~ConfigManager() {
    if (dirty) {
        save();
    }
}

std::string ConfigManager::determine_default_config_path() {
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base_path;
    if (xdg_config && std::string(xdg_config).length() > 0) {
        base_path = std::filesystem::path(xdg_config) / "vmp";
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            base_path = std::filesystem::path(home) / ".config" / "vmp";
        } else {
            base_path = ".vmp";
        }
    }

    try {
        std::filesystem::create_directories(base_path);
    } catch (...) {}

    return (base_path / "config.ini").string();
}

bool ConfigManager::load(const std::string& custom_path) {
    std::lock_guard<std::mutex> lock(config_mutex);
    if (!custom_path.empty()) {
        config_path = custom_path;
    }

    std::ifstream file(config_path);
    if (!file.is_open()) {
        // Apply sensible defaults
        settings["audio"]["volume"] = "1.0";
        settings["audio"]["muted"] = "false";
        settings["video"]["hw_accel"] = "auto";
        settings["video"]["hdr_tone_mapping"] = "false";
        settings["video"]["default_shader"] = "0";
        settings["video"]["vsync"] = "true";
        settings["subtitles"]["enabled"] = "true";
        settings["subtitles"]["delay_sec"] = "0.0";
        settings["subtitles"]["font_size"] = "28";
        settings["general"]["auto_resume"] = "true";
        dirty = true;
        return false;
    }

    std::string line;
    std::string current_section = "general";

    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = trim(trimmed.substr(1, trimmed.length() - 2));
            for (char& c : current_section) c = std::tolower(c);
            continue;
        }

        size_t eq_pos = trimmed.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = trim(trimmed.substr(0, eq_pos));
            std::string val = trim(trimmed.substr(eq_pos + 1));
            for (char& c : key) c = std::tolower(c);
            settings[current_section][key] = val;
        }
    }

    dirty = false;
    return true;
}

bool ConfigManager::save() {
    std::lock_guard<std::mutex> lock(config_mutex);
    try {
        std::filesystem::path dir = std::filesystem::path(config_path).parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir);
        }
    } catch (...) {}

    std::ofstream file(config_path, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    file << "# VMP (Video Max Player) Configuration File\n\n";

    for (const auto& [section, kv_pairs] : settings) {
        file << "[" << section << "]\n";
        for (const auto& [k, v] : kv_pairs) {
            file << k << "=" << v << "\n";
        }
        file << "\n";
    }

    dirty = false;
    return true;
}

std::string ConfigManager::get_string(const std::string& section, const std::string& key, const std::string& default_val) const {
    std::lock_guard<std::mutex> lock(config_mutex);
    std::string s = section;
    std::string k = key;
    for (char& c : s) c = std::tolower(c);
    for (char& c : k) c = std::tolower(c);

    auto sit = settings.find(s);
    if (sit != settings.end()) {
        auto kit = sit->second.find(k);
        if (kit != sit->second.end()) {
            return kit->second;
        }
    }
    return default_val;
}

void ConfigManager::set_string(const std::string& section, const std::string& key, const std::string& val) {
    std::lock_guard<std::mutex> lock(config_mutex);
    std::string s = section;
    std::string k = key;
    for (char& c : s) c = std::tolower(c);
    for (char& c : k) c = std::tolower(c);

    settings[s][k] = val;
    dirty = true;
}

int ConfigManager::get_int(const std::string& section, const std::string& key, int default_val) const {
    std::string val = get_string(section, key, "");
    if (val.empty()) return default_val;
    try {
        return std::stoi(val);
    } catch (...) {
        return default_val;
    }
}

void ConfigManager::set_int(const std::string& section, const std::string& key, int val) {
    set_string(section, key, std::to_string(val));
}

float ConfigManager::get_float(const std::string& section, const std::string& key, float default_val) const {
    std::string val = get_string(section, key, "");
    if (val.empty()) return default_val;
    try {
        return std::stof(val);
    } catch (...) {
        return default_val;
    }
}

void ConfigManager::set_float(const std::string& section, const std::string& key, float val) {
    set_string(section, key, std::to_string(val));
}

bool ConfigManager::get_bool(const std::string& section, const std::string& key, bool default_val) const {
    std::string val = get_string(section, key, "");
    if (val.empty()) return default_val;
    for (char& c : val) c = std::tolower(c);
    if (val == "true" || val == "1" || val == "yes" || val == "on") return true;
    if (val == "false" || val == "0" || val == "no" || val == "off") return false;
    return default_val;
}

void ConfigManager::set_bool(const std::string& section, const std::string& key, bool val) {
    set_string(section, key, val ? "true" : "false");
}
