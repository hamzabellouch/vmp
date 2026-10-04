#ifndef VMP_SUBTITLES_H
#define VMP_SUBTITLES_H

#include <string>
#include <vector>

struct SubtitleEntry {
    double start_time; // in seconds
    double end_time;   // in seconds
    std::string text;
    int style_flags = 0; // 1: italic, 2: bold, 4: top position
};

struct SubtitleTrack {
    std::string title;
    std::string filepath;
    std::vector<SubtitleEntry> entries;
    bool is_embedded = false;
};

class SubtitleParser {
public:
    static std::vector<SubtitleEntry> parse_srt(const std::string& filepath);
    static std::vector<SubtitleEntry> parse_ass(const std::string& filepath);
    static std::vector<SubtitleEntry> parse_file(const std::string& filepath);
    static std::vector<SubtitleTrack> find_subtitle_tracks_for_video(const std::string& video_path);
    static std::string get_active_subtitle(const std::vector<SubtitleEntry>& entries, double current_time, double delay_offset_sec = 0.0);
    static std::string clean_ass_formatting(const std::string& raw);
};

#endif // VMP_SUBTITLES_H
