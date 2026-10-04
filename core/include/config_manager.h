#ifndef VMP_CONFIG_MANAGER_H
#define VMP_CONFIG_MANAGER_H

#include <string>
#include <unordered_map>
#include <mutex>

class ConfigManager {
public:
    static ConfigManager& get_instance();

    bool load(const std::string& custom_path = "");
    bool save();

    std::string get_string(const std::string& section, const std::string& key, const std::string& default_val = "") const;
    void set_string(const std::string& section, const std::string& key, const std::string& val);

    int get_int(const std::string& section, const std::string& key, int default_val = 0) const;
    void set_int(const std::string& section, const std::string& key, int val);

    float get_float(const std::string& section, const std::string& key, float default_val = 0.0f) const;
    void set_float(const std::string& section, const std::string& key, float val);

    bool get_bool(const std::string& section, const std::string& key, bool default_val = false) const;
    void set_bool(const std::string& section, const std::string& key, bool val);

    std::string get_config_file_path() const { return config_path; }

private:
    ConfigManager();
    ~ConfigManager();

    std::string determine_default_config_path();

    mutable std::mutex config_mutex;
    std::string config_path;
    // section -> (key -> value)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> settings;
    bool dirty = false;
};

#endif // VMP_CONFIG_MANAGER_H
