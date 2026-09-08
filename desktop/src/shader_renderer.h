#ifndef VMP_SHADER_RENDERER_H
#define VMP_SHADER_RENDERER_H

#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <GLFW/glfw3.h>

struct FolderMediaItem {
    std::string filepath;
    std::string filename;
    std::string size_str;
    std::string date_str;
    std::string duration_str;
    double duration_sec = 0.0;
    GLuint thumb_tex = 0;
    int thumb_w = 0;
    int thumb_h = 0;
};

enum class VlcMenuAction {
    NONE = 0,
    MEDIA_OPEN_FILE,
    MEDIA_OPEN_FOLDER,
    MEDIA_QUIT,
    PLAYBACK_TOGGLE_PAUSE,
    PLAYBACK_FORWARD,
    PLAYBACK_REWIND,
    AUDIO_VOL_UP,
    AUDIO_VOL_DOWN,
    AUDIO_MUTE,
    VIDEO_FULLSCREEN,
    VIDEO_ASPECT_RATIO,
    SUBTITLE_TOGGLE,
    SUBTITLE_DELAY_MINUS,
    SUBTITLE_DELAY_PLUS,
    TOOLS_STATS,
    VIEW_GALLERY,
    HELP_ABOUT
};

struct VlcMenuItem {
    std::string label;
    std::string shortcut;
    VlcMenuAction action = VlcMenuAction::NONE;
};

struct VlcMenuCategory {
    std::string title;
    std::vector<VlcMenuItem> items;
    float x = 0.0f;
    float width = 0.0f;
};

class ShaderRenderer {
public:
    enum class RenderMode {
        FIT,
        PIXEL_PERFECT,
        STRETCH
    };

    enum class AspectRatioMode {
        AUTO,       // Original Aspect Ratio (from video metadata)
        RATIO_16_9, // 16:9 Widescreen
        RATIO_4_3,  // 4:3 Classic TV
        RATIO_21_9, // 21:9 Ultrawide / CinemaScope
        RATIO_1_1,  // 1:1 Square
        RATIO_FILL  // Fill window frame
    };

    ShaderRenderer();
    ~ShaderRenderer();

    bool init_gl_shaders();
    void upload_yuv_frame(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                          int y_stride, int u_stride, int v_stride,
                          int width, int height);
    void upload_nv12_frame(uint8_t* y_plane, uint8_t* uv_plane,
                           int y_stride, int uv_stride,
                           int width, int height);
    void upload_p010_frame(uint8_t* y_plane, uint8_t* uv_plane,
                           int y_stride, int uv_stride,
                           int width, int height);
    void upload_yuv10_frame(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                            int y_stride, int u_stride, int v_stride,
                            int width, int height);
    void upload_rgb_frame(uint8_t* rgb_data, int width, int height);
    void upload_thumbnail_frame(const uint8_t* rgb_data, int width, int height);
    
    // Zoom and Pan transformations
    void set_transform(float zoom, float pan_x, float pan_y);
    void set_hdr_tone_mapping(bool enable) { hdr_enabled = enable; }
    void set_sharpness_filter(bool enable) { sharpness_enabled = enable; }
    bool is_sharpness_filter_enabled() const { return sharpness_enabled; }
    void set_sharpness_strength(float strength) { sharpness_strength = strength; }
    float get_sharpness_strength() const { return sharpness_strength; }
    void set_render_mode(RenderMode mode) { render_mode = mode; }
    RenderMode get_render_mode() const { return render_mode; }
    void set_aspect_ratio_mode(AspectRatioMode mode) { aspect_ratio_mode = mode; }
    AspectRatioMode get_aspect_ratio_mode() const { return aspect_ratio_mode; }
    std::string get_aspect_ratio_name() const {
        switch (aspect_ratio_mode) {
            case AspectRatioMode::AUTO: return "Auto (Original)";
            case AspectRatioMode::RATIO_16_9: return "16:9";
            case AspectRatioMode::RATIO_4_3: return "4:3";
            case AspectRatioMode::RATIO_21_9: return "21:9 (Ultrawide)";
            case AspectRatioMode::RATIO_1_1: return "1:1";
            case AspectRatioMode::RATIO_FILL: return "Fill (Stretch)";
            default: return "Auto";
        }
    }
    
    // GPU Color Grading
    void set_brightness(float b) { brightness = std::max(-0.5f, std::min(0.5f, b)); }
    float get_brightness() const { return brightness; }
    void set_contrast(float c) { contrast = std::max(0.2f, std::min(3.0f, c)); }
    float get_contrast() const { return contrast; }
    void set_saturation(float s) { saturation = std::max(0.0f, std::min(3.0f, s)); }
    float get_saturation() const { return saturation; }
    void reset_color_grading() { brightness = 0.0f; contrast = 1.0f; saturation = 1.0f; }

    static bool save_framebuffer_screenshot(int width, int height, const std::string& filepath);
    
    void render(int window_width, int window_height);
    void render_ui_overlay(int window_width, int window_height, double current_sec, double total_sec, 
                           bool is_paused, bool is_fullscreen, float ui_alpha, double mouse_x, double mouse_y, 
                           bool is_scrubbing, bool show_stats, const std::string& video_name, 
                           const std::string& video_size, const std::string& video_date, 
                           const std::string& video_resolution, const std::string& codec_name, 
                           double fps, const std::string& hw_status, const std::string& active_subtitle,
                           const std::string& osd_notification);
    void render_welcome_screen(int window_width, int window_height, double mouse_x, double mouse_y);
    void render_folder_gallery(int window_width, int window_height, double mouse_x, double mouse_y,
                               const std::string& folder_path, const std::vector<FolderMediaItem>& items,
                               int scroll_offset, bool is_fullscreen = false);
    static GLuint create_rgb_texture(const uint8_t* rgb_data, int width, int height);
    float get_text_width(const std::string& text, float font_size);

    void render_vlc_menu_bar(int window_width, int window_height, double mouse_x, double mouse_y,
                            int active_menu_idx, float menu_alpha = 1.0f, const std::string& title = "");
    void render_vlc_dropdown(int window_width, int window_height, double mouse_x, double mouse_y,
                            int menu_idx);
    int hit_test_vlc_menu_bar(int window_width, int window_height, double mouse_x, double mouse_y);
    VlcMenuAction hit_test_vlc_dropdown(int window_width, int window_height, double mouse_x, double mouse_y, int menu_idx);
    bool is_mouse_inside_dropdown(int window_width, int window_height, double mouse_x, double mouse_y, int menu_idx);

private:
    std::vector<VlcMenuCategory> vlc_menus;
    void init_vlc_menus();
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint shader_program = 0;
    
    GLuint ui_shader_program = 0;
    GLuint ui_vao = 0;
    GLuint ui_vbo = 0;

    GLuint tex_icon_play = 0;
    GLuint tex_icon_pause = 0;
    GLuint tex_icon_replay5 = 0;
    GLuint tex_icon_forward5 = 0;
    GLuint tex_icon_fullscreen = 0;
    GLuint tex_icon_fullscreen_exit = 0;
    GLuint tex_icon_indicator = 0;
    GLuint tex_icon_stats = 0;

    struct GlyphTexture {
        GLuint texture = 0;
        int width = 0;
        int height = 0;
        int bearing_x = 0;
        int bearing_y = 0;
        float advance = 0.0f;
    };

    void* ft_library = nullptr;
    void* ft_face = nullptr;
    void* ft_fallback_face = nullptr;
    bool font_initialized = false;
    std::unordered_map<uint64_t, GlyphTexture> glyph_cache;

    bool init_font_engine();
    void cleanup_font_engine();
    const GlyphTexture* get_glyph(uint32_t codepoint, int font_size);
    void draw_ui_text(const std::string& text, float x, float y, float font_size, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h);
    
    GLuint y_texture = 0;
    GLuint u_texture = 0;
    GLuint v_texture = 0;
    GLuint uv_texture = 0;
    GLuint thumbnail_texture = 0;
    bool has_thumbnail_texture = false;
    int thumbnail_width = 160;
    int thumbnail_height = 90;
    
    int current_width = 0;
    int current_height = 0;
    int allocated_width = 0;
    int allocated_height = 0;
    int format_mode = 0; // 0: YUV420P, 1: NV12, 2: RGB
    int allocated_format = -1;
    bool hdr_enabled = false;
    bool sharpness_enabled = true; // CAS GPU Sharpness Filter enabled by default for ultra-clarity
    float sharpness_strength = 0.65f;
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    RenderMode render_mode = RenderMode::FIT;
    AspectRatioMode aspect_ratio_mode = AspectRatioMode::AUTO;

    float zoom_scale = 1.0f;
    float pan_offset_x = 0.0f;
    float pan_offset_y = 0.0f;

    GLuint compile_shader(GLenum type, const char* source);
    void draw_ui_rect(float x, float y, float w, float h, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h);
    void draw_ui_rounded_rect(float x, float y, float w, float h, float radius, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h);
    void draw_ui_circle(float cx, float cy, float radius, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h);
    void draw_ui_icon(GLuint tex, float x, float y, float w, float h, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h);
    void draw_ui_string(const std::string& text, float x, float y, float char_w, float char_h, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h);
};

#endif // VMP_SHADER_RENDERER_H
