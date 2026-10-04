#include "mpris_manager.h"
#include <iostream>
#include <unistd.h>

#if defined(VMP_HAS_DBUS)

static const char* MPRIS_INTROSPECT_XML =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"data\" direction=\"out\" type=\"s\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"property_name\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"value\" direction=\"out\" type=\"v\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"properties\" direction=\"out\" type=\"a{sv}\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.mpris.MediaPlayer2\">\n"
    "    <method name=\"Raise\"/>\n"
    "    <method name=\"Quit\"/>\n"
    "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"DesktopEntry\" type=\"s\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
    "    <method name=\"Next\"/>\n"
    "    <method name=\"Previous\"/>\n"
    "    <method name=\"Pause\"/>\n"
    "    <method name=\"PlayPause\"/>\n"
    "    <method name=\"Stop\"/>\n"
    "    <method name=\"Play\"/>\n"
    "    <method name=\"Seek\">\n"
    "      <arg name=\"Offset\" direction=\"in\" type=\"x\"/>\n"
    "    </method>\n"
    "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Rate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
    "    <property name=\"Volume\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Position\" type=\"x\" access=\"read\"/>\n"
    "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
    "  </interface>\n"
    "</node>\n";

DBusHandlerResult MprisManager::message_filter(DBusConnection* connection, DBusMessage* message, void* user_data) {
    MprisManager* self = static_cast<MprisManager*>(user_data);
    if (!self) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    return self->handle_message(connection, message);
}

DBusHandlerResult MprisManager::handle_message(DBusConnection* connection, DBusMessage* message) {
    const char* path = dbus_message_get_path(message);
    if (!path || std::string(path) != "/org/mpris/MediaPlayer2") {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char* iface = dbus_message_get_interface(message);
    const char* member = dbus_message_get_member(message);
    if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    std::string s_iface = iface;
    std::string s_member = member;

    if (s_iface == "org.freedesktop.DBus.Introspectable" && s_member == "Introspect") {
        DBusMessage* reply = dbus_message_new_method_return(message);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &MPRIS_INTROSPECT_XML, DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (s_iface == "org.mpris.MediaPlayer2") {
        if (s_member == "Raise") {
            if (cbs.on_raise) cbs.on_raise();
        } else if (s_member == "Quit") {
            if (cbs.on_quit) cbs.on_quit();
        }
        DBusMessage* reply = dbus_message_new_method_return(message);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (s_iface == "org.mpris.MediaPlayer2.Player") {
        if (s_member == "Play") {
            if (cbs.on_play) cbs.on_play();
        } else if (s_member == "Pause") {
            if (cbs.on_pause) cbs.on_pause();
        } else if (s_member == "PlayPause") {
            if (cbs.on_play_pause) cbs.on_play_pause();
        } else if (s_member == "Stop") {
            if (cbs.on_stop) cbs.on_stop();
        } else if (s_member == "Next") {
            if (cbs.on_next) cbs.on_next();
        } else if (s_member == "Previous") {
            if (cbs.on_prev) cbs.on_prev();
        } else if (s_member == "Seek") {
            dbus_int64_t offset_usec = 0;
            if (dbus_message_get_args(message, nullptr, DBUS_TYPE_INT64, &offset_usec, DBUS_TYPE_INVALID)) {
                if (cbs.on_seek_relative) cbs.on_seek_relative(static_cast<double>(offset_usec) / 1000000.0);
            }
        }
        DBusMessage* reply = dbus_message_new_method_return(message);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (s_iface == "org.freedesktop.DBus.Properties") {
        if (s_member == "Get") {
            const char* req_iface = nullptr;
            const char* req_prop = nullptr;
            if (dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &req_iface, DBUS_TYPE_STRING, &req_prop, DBUS_TYPE_INVALID)) {
                handle_get_property(connection, message, req_iface, req_prop);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
        } else if (s_member == "GetAll") {
            const char* req_iface = nullptr;
            if (dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &req_iface, DBUS_TYPE_INVALID)) {
                handle_get_all_properties(connection, message, req_iface);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
        }
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void MprisManager::handle_get_property(DBusConnection* connection, DBusMessage* message, const char* iface, const char* prop) {
    DBusMessage* reply = dbus_message_new_method_return(message);
    DBusMessageIter iter, var_iter;
    dbus_message_iter_init_append(reply, &iter);

    std::string s_prop = prop ? prop : "";

    if (s_prop == "PlaybackStatus") {
        const char* status_str = current_status.c_str();
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &var_iter);
        dbus_message_iter_append_basic(&var_iter, DBUS_TYPE_STRING, &status_str);
        dbus_message_iter_close_container(&iter, &var_iter);
    } else if (s_prop == "Identity") {
        const char* id = "VMP (Video Max Player)";
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &var_iter);
        dbus_message_iter_append_basic(&var_iter, DBUS_TYPE_STRING, &id);
        dbus_message_iter_close_container(&iter, &var_iter);
    } else if (s_prop == "DesktopEntry") {
        const char* de = "vmp";
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &var_iter);
        dbus_message_iter_append_basic(&var_iter, DBUS_TYPE_STRING, &de);
        dbus_message_iter_close_container(&iter, &var_iter);
    } else if (s_prop == "CanPlay" || s_prop == "CanPause" || s_prop == "CanSeek" || s_prop == "CanControl" ||
               s_prop == "CanQuit" || s_prop == "CanRaise") {
        dbus_bool_t val = TRUE;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &var_iter);
        dbus_message_iter_append_basic(&var_iter, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&iter, &var_iter);
    } else if (s_prop == "Volume") {
        double vol = current_vol;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "d", &var_iter);
        dbus_message_iter_append_basic(&var_iter, DBUS_TYPE_DOUBLE, &vol);
        dbus_message_iter_close_container(&iter, &var_iter);
    } else if (s_prop == "Position") {
        dbus_int64_t pos_usec = static_cast<dbus_int64_t>(current_pos * 1000000.0);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "x", &var_iter);
        dbus_message_iter_append_basic(&var_iter, DBUS_TYPE_INT64, &pos_usec);
        dbus_message_iter_close_container(&iter, &var_iter);
    } else {
        dbus_bool_t val = FALSE;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &var_iter);
        dbus_message_iter_append_basic(&var_iter, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&iter, &var_iter);
    }

    dbus_connection_send(connection, reply, nullptr);
    dbus_message_unref(reply);
}

void MprisManager::handle_get_all_properties(DBusConnection* connection, DBusMessage* message, const char* iface) {
    (void)iface;
    DBusMessage* reply = dbus_message_new_method_return(message);
    DBusMessageIter iter, array_iter, dict_iter, var_iter;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &array_iter);

    auto add_string_prop = [&](const char* key, const char* val) {
        dbus_message_iter_open_container(&array_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &dict_iter);
        dbus_message_iter_append_basic(&dict_iter, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_VARIANT, "s", &var_iter);
        dbus_message_iter_append_basic(&var_iter, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&dict_iter, &var_iter);
        dbus_message_iter_close_container(&array_iter, &dict_iter);
    };

    auto add_bool_prop = [&](const char* key, dbus_bool_t val) {
        dbus_message_iter_open_container(&array_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &dict_iter);
        dbus_message_iter_append_basic(&dict_iter, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_VARIANT, "b", &var_iter);
        dbus_message_iter_append_basic(&var_iter, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&dict_iter, &var_iter);
        dbus_message_iter_close_container(&array_iter, &dict_iter);
    };

    add_string_prop("PlaybackStatus", current_status.c_str());
    add_string_prop("Identity", "VMP (Video Max Player)");
    add_string_prop("DesktopEntry", "vmp");
    add_bool_prop("CanPlay", TRUE);
    add_bool_prop("CanPause", TRUE);
    add_bool_prop("CanSeek", TRUE);
    add_bool_prop("CanControl", TRUE);
    add_bool_prop("CanQuit", TRUE);
    add_bool_prop("CanRaise", TRUE);

    dbus_message_iter_close_container(&iter, &array_iter);
    dbus_connection_send(connection, reply, nullptr);
    dbus_message_unref(reply);
}

#endif // VMP_HAS_DBUS

MprisManager::MprisManager() {}

MprisManager::~MprisManager() {
#if defined(VMP_HAS_DBUS)
    if (conn) {
        dbus_connection_unref(conn);
        conn = nullptr;
    }
#endif
}

bool MprisManager::init(const Callbacks& callbacks) {
    cbs = callbacks;
#if defined(VMP_HAS_DBUS)
    DBusError err;
    dbus_error_init(&err);

    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err) || !conn) {
        std::cerr << "[VMP MPRIS] Could not connect to D-Bus session bus: " << (err.message ? err.message : "unknown") << std::endl;
        dbus_error_free(&err);
        return false;
    }

    std::string service_name = "org.mpris.MediaPlayer2.vmp";
    int ret = dbus_bus_request_name(conn, service_name.c_str(), DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        // Primary name already owned; use instance suffix
        service_name = "org.mpris.MediaPlayer2.vmp.instance" + std::to_string(getpid());
        dbus_error_free(&err);
        dbus_error_init(&err);
        ret = dbus_bus_request_name(conn, service_name.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    }

    if (dbus_error_is_set(&err)) {
        std::cerr << "[VMP MPRIS] Failed to request bus name: " << (err.message ? err.message : "") << std::endl;
        dbus_error_free(&err);
        return false;
    }

    dbus_connection_add_filter(conn, message_filter, this, nullptr);
    initialized = true;
    std::cout << "[VMP Engine] MPRIS2 Desktop Media Controls active on D-Bus: " << service_name << std::endl;
    return true;
#else
    return false;
#endif
}

void MprisManager::update() {
#if defined(VMP_HAS_DBUS)
    if (conn) {
        dbus_connection_read_write_dispatch(conn, 0);
    }
#endif
}

void MprisManager::update_playback_status(const std::string& status) {
#if defined(VMP_HAS_DBUS)
    current_status = status;
#endif
}

void MprisManager::update_metadata(const std::string& title, double duration_sec, const std::string& file_uri) {
#if defined(VMP_HAS_DBUS)
    current_title = title;
    current_duration = duration_sec;
    (void)file_uri;
#endif
}

void MprisManager::update_position(double position_sec) {
#if defined(VMP_HAS_DBUS)
    current_pos = position_sec;
#endif
}

void MprisManager::update_volume(float volume) {
#if defined(VMP_HAS_DBUS)
    current_vol = volume;
#endif
}
