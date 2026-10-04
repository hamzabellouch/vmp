#include "subtitles.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <regex>

static double parse_srt_time(const std::string& time_str) {
    int hrs = 0, mins = 0, secs = 0, ms = 0;
    // Standard SRT with comma: HH:MM:SS,mmm
    if (sscanf(time_str.c_str(), "%d:%d:%d,%d", &hrs, &mins, &secs, &ms) == 4) {
        return hrs * 3600.0 + mins * 60.0 + secs + ms / 1000.0;
    }
    // SRT / VTT with dot: HH:MM:SS.mmm
    if (sscanf(time_str.c_str(), "%d:%d:%d.%d", &hrs, &mins, &secs, &ms) == 4) {
        return hrs * 3600.0 + mins * 60.0 + secs + ms / 1000.0;
    }
    // Short format: MM:SS.mmm or MM:SS,mmm
    if (sscanf(time_str.c_str(), "%d:%d.%d", &mins, &secs, &ms) == 3) {
        return mins * 60.0 + secs + ms / 1000.0;
    }
    if (sscanf(time_str.c_str(), "%d:%d,%d", &mins, &secs, &ms) == 3) {
        return mins * 60.0 + secs + ms / 1000.0;
    }
    return 0.0;
}

static double parse_ass_time(const std::string& time_str) {
    int hrs = 0, mins = 0, secs = 0, cs = 0;
    if (sscanf(time_str.c_str(), "%d:%d:%d.%d", &hrs, &mins, &secs, &cs) == 4) {
        return hrs * 3600.0 + mins * 60.0 + secs + cs / 100.0;
    }
    return 0.0;
}

std::string SubtitleParser::clean_ass_formatting(const std::string& raw) {
    std::string result = "";
    bool in_tag = false;

    for (size_t i = 0; i < raw.length(); i++) {
        if (raw[i] == '{') {
            in_tag = true;
        } else if (raw[i] == '}') {
            in_tag = false;
        } else if (!in_tag) {
            // Handle ASS newline escapes \N and \n
            if (raw[i] == '\\' && i + 1 < raw.length() && (raw[i + 1] == 'N' || raw[i + 1] == 'n')) {
                result += " ";
                i++;
            } else if (raw[i] == '\\' && i + 1 < raw.length() && raw[i + 1] == 'h') {
                result += " ";
                i++;
            } else {
                result += raw[i];
            }
        }
    }
    return result;
}

std::vector<SubtitleEntry> SubtitleParser::parse_srt(const std::string& filepath) {
    std::vector<SubtitleEntry> entries;
    std::ifstream file(filepath);
    if (!file.is_open()) return entries;

    std::string line;
    enum State { INDEX, TIMECODE, TEXT };
    State state = INDEX;
    SubtitleEntry current_entry;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (state == INDEX) {
            if (line.empty()) continue;
            state = TIMECODE;
        } else if (state == TIMECODE) {
            size_t arrow_pos = line.find("-->");
            if (arrow_pos != std::string::npos) {
                std::string start_str = line.substr(0, arrow_pos);
                std::string end_str = line.substr(arrow_pos + 3);

                start_str.erase(0, start_str.find_first_not_of(" \t"));
                start_str.erase(start_str.find_last_not_of(" \t") + 1);
                end_str.erase(0, end_str.find_first_not_of(" \t"));
                end_str.erase(end_str.find_last_not_of(" \t") + 1);

                current_entry.start_time = parse_srt_time(start_str);
                current_entry.end_time = parse_srt_time(end_str);
                current_entry.text = "";
                state = TEXT;
            } else {
                state = INDEX;
            }
        } else if (state == TEXT) {
            if (line.empty()) {
                if (!current_entry.text.empty()) {
                    entries.push_back(current_entry);
                }
                state = INDEX;
            } else {
                if (!current_entry.text.empty()) {
                    current_entry.text += " ";
                }
                current_entry.text += line;
            }
        }
    }

    if (state == TEXT && !current_entry.text.empty()) {
        entries.push_back(current_entry);
    }

    return entries;
}

std::vector<SubtitleEntry> SubtitleParser::parse_ass(const std::string& filepath) {
    std::vector<SubtitleEntry> entries;
    std::ifstream file(filepath);
    if (!file.is_open()) return entries;

    std::string line;
    bool in_events = false;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "[Events]") {
            in_events = true;
            continue;
        }

        if (in_events && (line.rfind("Dialogue:", 0) == 0)) {
            // Dialogue format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            std::string content = line.substr(9);
            content.erase(0, content.find_first_not_of(" \t"));

            std::vector<std::string> fields;
            size_t start = 0;
            for (int i = 0; i < 9; i++) {
                size_t comma = content.find(',', start);
                if (comma == std::string::npos) break;
                fields.push_back(content.substr(start, comma - start));
                start = comma + 1;
            }
            // The 10th field is the entire remaining text
            if (start < content.length()) {
                fields.push_back(content.substr(start));
            }

            if (fields.size() >= 10) {
                SubtitleEntry entry;
                entry.start_time = parse_ass_time(fields[1]);
                entry.end_time = parse_ass_time(fields[2]);
                entry.text = clean_ass_formatting(fields[9]);

                if (!entry.text.empty()) {
                    entries.push_back(entry);
                }
            }
        }
    }

    // Sort by start time
    std::sort(entries.begin(), entries.end(), [](const SubtitleEntry& a, const SubtitleEntry& b) {
        return a.start_time < b.start_time;
    });

    return entries;
}

std::vector<SubtitleEntry> SubtitleParser::parse_file(const std::string& filepath) {
    std::string ext = "";
    size_t dot_pos = filepath.find_last_of('.');
    if (dot_pos != std::string::npos) {
        ext = filepath.substr(dot_pos);
        for (char& c : ext) c = std::tolower(c);
    }

    if (ext == ".ass" || ext == ".ssa") {
        return parse_ass(filepath);
    }
    return parse_srt(filepath);
}

std::vector<SubtitleTrack> SubtitleParser::find_subtitle_tracks_for_video(const std::string& video_path) {
    std::vector<SubtitleTrack> tracks;
    if (video_path.empty()) return tracks;

    std::filesystem::path v_path(video_path);
    std::string stem = v_path.stem().string();
    std::filesystem::path dir = v_path.parent_path();
    if (dir.empty()) dir = ".";

    if (!std::filesystem::exists(dir)) return tracks;

    // Check direct matching subtitle files
    std::vector<std::string> candidates = {
        (dir / (stem + ".ass")).string(),
        (dir / (stem + ".ssa")).string(),
        (dir / (stem + ".srt")).string(),
        (dir / (stem + ".en.ass")).string(),
        (dir / (stem + ".en.srt")).string(),
        (dir / (stem + ".ar.ass")).string(),
        (dir / (stem + ".ar.srt")).string()
    };

    for (const auto& cand : candidates) {
        if (std::filesystem::exists(cand)) {
            SubtitleTrack track;
            track.filepath = cand;
            track.title = std::filesystem::path(cand).filename().string();
            track.entries = parse_file(cand);
            if (!track.entries.empty()) {
                tracks.push_back(track);
            }
        }
    }

    return tracks;
}

std::string SubtitleParser::get_active_subtitle(const std::vector<SubtitleEntry>& entries, double current_time, double delay_offset_sec) {
    double adjusted_time = current_time - delay_offset_sec;
    for (const auto& entry : entries) {
        if (adjusted_time >= entry.start_time && adjusted_time <= entry.end_time) {
            return entry.text;
        }
    }
    return "";
}
