#ifndef VMP_MPRIS_MANAGER_H
#define VMP_MPRIS_MANAGER_H

#include <string>
#include <functional>

#if defined(VMP_HAS_DBUS)
#include <dbus/dbus.h>
#endif

class MprisManager {
public:
    struct Callbacks {
        std::function<void()> on_play;
        std::function<void()> on_pause;
        std::function<void()> on_play_pause;
        std::function<void()> on_stop;
        std::function<void()> on_next;
        std::function<void()> on_prev;
        std::function<void(double)> on_seek_relative; // in seconds
        std::function<void(double)> on_seek_absolute; // in seconds
        std::function<void(float)> on_set_volume;     // 0.0 to 1.5
        std::function<void()> on_raise;
        std::function<void()> on_quit;
    };

    MprisManager();
    ~MprisManager();

    bool init(const Callbacks& callbacks);
    void update(); // Process pending D-Bus events

    void update_playback_status(const std::string& status); // "Playing", "Paused", "Stopped"
    void update_metadata(const std::string& title, double duration_sec, const std::string& file_uri = "");
    void update_position(double position_sec);
    void update_volume(float volume);

private:
    Callbacks cbs;
    bool initialized = false;

#if defined(VMP_HAS_DBUS)
    DBusConnection* conn = nullptr;
    std::string current_status = "Stopped";
    std::string current_title = "";
    double current_duration = 0.0;
    double current_pos = 0.0;
    float current_vol = 1.0f;

    static DBusHandlerResult message_filter(DBusConnection* connection, DBusMessage* message, void* user_data);
    DBusHandlerResult handle_message(DBusConnection* connection, DBusMessage* message);
    void handle_get_property(DBusConnection* connection, DBusMessage* message, const char* iface, const char* prop);
    void handle_get_all_properties(DBusConnection* connection, DBusMessage* message, const char* iface);
#endif
};

#endif // VMP_MPRIS_MANAGER_H
