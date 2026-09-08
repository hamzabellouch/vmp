#include "shader_renderer.h"
#include "material_icons_data.h"
#include "seek_icons_data.h"
#include "font_data.h"
#include <vector>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fribidi.h>

static std::string fmt_time_short(double sec, bool force_hours = false) {
    int total = static_cast<int>(std::max(0.0, sec));
    int m = (total % 3600) / 60;
    int s = total % 60;
    int h = total / 3600;
    char buf[32];
    if (h > 0 || force_hours) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    }
    return std::string(buf);
}

// Vertex Shader with Zoom and Pan Matrix transformation
static const char* vertex_shader_src = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

uniform vec2 uScale;
uniform float uZoom;
uniform vec2 uPan;

void main() {
    vec2 pos = (aPos * uScale * uZoom) + uPan;
    gl_Position = vec4(pos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

// Fragment Shader with BT.709 / BT.2020 YUV -> RGB + GPU CAS Adaptive Sharpening + ACES Filmic Tone Mapping
static const char* fragment_shader_src = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;
uniform sampler2D texUV;
uniform int formatMode; // 0 = YUV420P / YUV10, 1 = NV12 / P010, 2 = RGB / RGBA
uniform bool enableHDR;
uniform bool enableSharpness;
uniform float sharpnessStrength;
uniform vec2 uTexSize;
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;

// Sample YUV/RGB to standard linear RGB color
vec3 sample_rgb(vec2 uv) {
    if (formatMode == 0 || formatMode == 3) {
        // Planar YUV420P (8-bit) or YUV420P10 (10-bit planar)
        // 10-bit samples are in 0..1023; when uploaded to GL_R16, GL divides by 65535,
        // so scaling by (65535.0 / 1023.0) ~ 64.06158 restores the [0, 1] range.
        float scale = (formatMode == 3) ? 64.06158 : 1.0;
        float y = (clamp(texture(texY, uv).r * scale, 0.0, 1.0) - 0.062745) * 1.16438;
        float u = (clamp(texture(texU, uv).r * scale, 0.0, 1.0) - 0.5) * 1.13839;
        float v = (clamp(texture(texV, uv).r * scale, 0.0, 1.0) - 0.5) * 1.13839;

        if (enableHDR) {
            // BT.2020 Limited to RGB Matrix
            float r = y + 1.4746 * v;
            float g = y - 0.16455 * u - 0.57135 * v;
            float b = y + 1.8814 * u;
            return clamp(vec3(r, g, b), 0.0, 1.0);
        } else {
            // BT.709 Limited to RGB Matrix
            float r = y + 1.5748 * v;
            float g = y - 0.1873 * u - 0.4681 * v;
            float b = y + 1.8556 * u;
            return clamp(vec3(r, g, b), 0.0, 1.0);
        }
    } else if (formatMode == 1) {
        // NV12 / P010: Y plane + Interleaved UV plane
        float y = (texture(texY, uv).r - 0.062745) * 1.16438;
        vec2 uv_val = texture(texUV, uv).rg;
        float u = (uv_val.r - 0.5) * 1.13839;
        float v = (uv_val.g - 0.5) * 1.13839;

        if (enableHDR) {
            // BT.2020 Limited to RGB Matrix
            float r = y + 1.4746 * v;
            float g = y - 0.16455 * u - 0.57135 * v;
            float b = y + 1.8814 * u;
            return clamp(vec3(r, g, b), 0.0, 1.0);
        } else {
            // BT.709 Limited to RGB Matrix
            float r = y + 1.5748 * v;
            float g = y - 0.1873 * u - 0.4681 * v;
            float b = y + 1.8556 * u;
            return clamp(vec3(r, g, b), 0.0, 1.0);
        }
    } else {
        return texture(texY, uv).rgb;
    }
}

// Contrast-Adaptive Sharpening (CAS) Filter
vec3 apply_cas(vec2 uv) {
    vec2 inv_size = vec2(1.0 / max(1.0, uTexSize.x), 1.0 / max(1.0, uTexSize.y));

    vec3 c = sample_rgb(uv);
    vec3 n = sample_rgb(uv + vec2(0.0, -inv_size.y));
    vec3 s = sample_rgb(uv + vec2(0.0, inv_size.y));
    vec3 w = sample_rgb(uv + vec2(-inv_size.x, 0.0));
    vec3 e = sample_rgb(uv + vec2(inv_size.x, 0.0));

    // Perceived luminance coefficients (BT.709) for consistent edge contrast
    const vec3 luma_weights = vec3(0.2126, 0.7152, 0.0722);
    float c_luma = dot(c, luma_weights);
    float n_luma = dot(n, luma_weights);
    float s_luma = dot(s, luma_weights);
    float w_luma = dot(w, luma_weights);
    float e_luma = dot(e, luma_weights);

    float min_luma = min(c_luma, min(min(n_luma, s_luma), min(w_luma, e_luma)));
    float max_luma = max(c_luma, max(max(n_luma, s_luma), max(w_luma, e_luma)));
    float diff = max_luma - min_luma;

    // Noise and subtle gradient floor: prevent amplifying smooth fog, smoke, gradients,
    // and compression block boundaries into coarse artifacts.
    if (diff < 0.008) {
        return c;
    }

    // Adaptive contrast weight based on distance to limits
    float amp = clamp(min(min_luma, 1.0 - max_luma) / (diff + 0.0001), 0.0, 1.0);
    // Smooth transition near noise floor to avoid harsh transition lines
    float fade = smoothstep(0.008, 0.04, diff);
    // Max sharpening weight clamped cleanly to prevent ringing and denominator collapse
    float w_cas = -sqrt(amp) * (sharpnessStrength * 0.18) * fade;

    vec3 sharp_rgb = (c + (n + s + w + e) * w_cas) / (1.0 + 4.0 * w_cas);
    return clamp(sharp_rgb, 0.0, 1.0);
}

// Color Grading adjustment (Brightness, Contrast, Saturation)
vec3 apply_color_grading(vec3 col) {
    col += uBrightness;
    col = (col - 0.5) * uContrast + 0.5;
    float gray = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(gray), col, uSaturation);
    return clamp(col, 0.0, 1.0);
}

// ACES Filmic Tone Mapping Curve for High Dynamic Range content
vec3 hdr_tone_map(vec3 x) {
    if (!enableHDR) return x;
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 rgb;
    if (enableSharpness && sharpnessStrength > 0.01 && uTexSize.x > 1.0) {
        rgb = apply_cas(TexCoord);
    } else {
        rgb = sample_rgb(TexCoord);
    }
    rgb = apply_color_grading(rgb);
    rgb = hdr_tone_map(rgb);
    FragColor = vec4(rgb, 1.0);
}
)";

ShaderRenderer::ShaderRenderer() {}

ShaderRenderer::~ShaderRenderer() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ui_vao) glDeleteVertexArrays(1, &ui_vao);
    if (ui_vbo) glDeleteBuffers(1, &ui_vbo);
    if (y_texture) glDeleteTextures(1, &y_texture);
    if (u_texture) glDeleteTextures(1, &u_texture);
    if (v_texture) glDeleteTextures(1, &v_texture);
    if (uv_texture) glDeleteTextures(1, &uv_texture);
    if (thumbnail_texture) glDeleteTextures(1, &thumbnail_texture);
    if (tex_icon_play) glDeleteTextures(1, &tex_icon_play);
    if (tex_icon_pause) glDeleteTextures(1, &tex_icon_pause);
    if (tex_icon_replay5) glDeleteTextures(1, &tex_icon_replay5);
    if (tex_icon_forward5) glDeleteTextures(1, &tex_icon_forward5);
    if (tex_icon_fullscreen) glDeleteTextures(1, &tex_icon_fullscreen);
    if (tex_icon_fullscreen_exit) glDeleteTextures(1, &tex_icon_fullscreen_exit);
    if (tex_icon_indicator) glDeleteTextures(1, &tex_icon_indicator);
    if (tex_icon_stats) glDeleteTextures(1, &tex_icon_stats);
    cleanup_font_engine();
    if (shader_program) glDeleteProgram(shader_program);
    if (ui_shader_program) glDeleteProgram(ui_shader_program);
}

GLuint ShaderRenderer::compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        std::cerr << "[VMP Shader Error]: " << infoLog << std::endl;
    }
    return shader;
}

static const char* ui_vertex_shader_src = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;
uniform vec2 uScreenSize;

void main() {
    vec2 ndc = (aPos / uScreenSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

static const char* ui_fragment_shader_src = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D uTex;
uniform bool uUseTex;
uniform bool uIsCircle;
uniform bool uIsRoundedRect;
uniform vec2 uRectSize;
uniform float uRadius;
uniform vec4 uColor;
uniform float uAlpha;

void main() {
    if (uIsCircle) {
        vec2 center = vec2(0.5, 0.5);
        float dist = distance(TexCoord, center);
        if (dist > 0.5) discard;
        float delta = fwidth(dist);
        float alpha = 1.0 - smoothstep(0.5 - delta, 0.5, dist);
        FragColor = vec4(uColor.rgb, uColor.a * uAlpha * alpha);
    } else if (uIsRoundedRect) {
        vec2 p = (TexCoord - 0.5) * uRectSize;
        float r = min(uRadius, min(uRectSize.x, uRectSize.y) * 0.5);
        vec2 b = uRectSize * 0.5 - vec2(r);
        vec2 q = abs(p) - b;
        float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        float delta = fwidth(dist);
        float alpha = 1.0 - smoothstep(-delta, delta, dist);
        if (alpha <= 0.0) discard;
        FragColor = vec4(uColor.rgb, uColor.a * uAlpha * alpha);
    } else if (uUseTex) {
        vec4 texColor = texture(uTex, TexCoord);
        FragColor = vec4(uColor.rgb * texColor.rgb, texColor.a * uColor.a * uAlpha);
    } else {
        FragColor = vec4(uColor.rgb, uColor.a * uAlpha);
    }
}
)";

bool ShaderRenderer::init_gl_shaders() {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);

    shader_program = glCreateProgram();
    glAttachShader(shader_program, vs);
    glAttachShader(shader_program, fs);
    glLinkProgram(shader_program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    // Setup UI Shaders
    GLuint uvs = compile_shader(GL_VERTEX_SHADER, ui_vertex_shader_src);
    GLuint ufs = compile_shader(GL_FRAGMENT_SHADER, ui_fragment_shader_src);
    ui_shader_program = glCreateProgram();
    glAttachShader(ui_shader_program, uvs);
    glAttachShader(ui_shader_program, ufs);
    glLinkProgram(ui_shader_program);
    glDeleteShader(uvs);
    glDeleteShader(ufs);

    glGenVertexArrays(1, &ui_vao);
    glGenBuffers(1, &ui_vbo);
    glBindVertexArray(ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    auto create_icon_tex = [](const uint8_t* rgba, int sz) {
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        return tex;
    };

    tex_icon_play = create_icon_tex(ICON_PLAY_RGBA, ICON_SIZE);
    tex_icon_pause = create_icon_tex(ICON_PAUSE_RGBA, ICON_SIZE);
    tex_icon_replay5 = create_icon_tex(ICON_REPLAY5_RGBA, ICON_SIZE);
    tex_icon_forward5 = create_icon_tex(ICON_FORWARD5_RGBA, ICON_SIZE);
    tex_icon_fullscreen = create_icon_tex(ICON_FULLSCREEN_RGBA, ICON_SIZE);
    tex_icon_fullscreen_exit = create_icon_tex(ICON_FULLSCREEN_EXIT_RGBA, ICON_SIZE);
    tex_icon_indicator = create_icon_tex(ICON_INDICATOR_RGBA, ICON_SIZE);
    tex_icon_stats = create_icon_tex(ICON_STATS_RGBA, ICON_SIZE);

    init_font_engine();

    float vertices[] = {
        // positions   // tex coords
        -1.0f,  1.0f,  0.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,

        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f
    };

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    GLuint textures[4];
    glGenTextures(4, textures);
    y_texture = textures[0];
    u_texture = textures[1];
    v_texture = textures[2];
    uv_texture = textures[3];

    auto setup_tex = [](GLuint tex) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };

    setup_tex(y_texture);
    setup_tex(u_texture);
    setup_tex(v_texture);
    setup_tex(uv_texture);

    clear_video_frame();

    std::cout << "[VMP Engine] Advanced GLSL NV12/YUV & HDR Tone Mapping Loaded." << std::endl;
    return true;
}

void ShaderRenderer::clear_video_frame() {
    has_video_frame = false;
    current_width = 0;
    current_height = 0;
    allocated_width = 0;
    allocated_height = 0;
    allocated_format = -1;

    // Reset textures with 1x1 black pixels (Y=16, U=128, V=128 in YUV space = Pure RGB Black)
    // This provides an impenetrable guard against green-screen artifacts in case of uninitialized sampling.
    uint8_t black_y = 16;
    uint8_t black_u = 128;
    uint8_t black_v = 128;
    uint8_t black_uv[2] = {128, 128};

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (y_texture) {
        glBindTexture(GL_TEXTURE_2D, y_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &black_y);
    }
    if (u_texture) {
        glBindTexture(GL_TEXTURE_2D, u_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &black_u);
    }
    if (v_texture) {
        glBindTexture(GL_TEXTURE_2D, v_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &black_v);
    }
    if (uv_texture) {
        glBindTexture(GL_TEXTURE_2D, uv_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, 1, 1, 0, GL_RG, GL_UNSIGNED_BYTE, black_uv);
    }
}

void ShaderRenderer::set_transform(float zoom, float pan_x, float pan_y) {
    zoom_scale = zoom;
    pan_offset_x = pan_x;
    pan_offset_y = pan_y;
}

void ShaderRenderer::upload_yuv_frame(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                      int y_stride, int u_stride, int v_stride,
                                      int width, int height) {
    format_mode = 0; // YUV420P
    current_width = width;
    current_height = height;

    bool size_changed = (width != allocated_width || height != allocated_height || allocated_format != 0);
    if (size_changed) {
        allocated_width = width;
        allocated_height = height;
        allocated_format = 0;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride);
    glBindTexture(GL_TEXTURE_2D, y_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, y_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, y_plane);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, u_stride);
    glBindTexture(GL_TEXTURE_2D, u_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, u_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RED, GL_UNSIGNED_BYTE, u_plane);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, v_stride);
    glBindTexture(GL_TEXTURE_2D, v_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, v_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RED, GL_UNSIGNED_BYTE, v_plane);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    has_video_frame = true;
}

void ShaderRenderer::upload_nv12_frame(uint8_t* y_plane, uint8_t* uv_plane,
                                       int y_stride, int uv_stride,
                                       int width, int height) {
    format_mode = 1; // NV12
    current_width = width;
    current_height = height;

    bool size_changed = (width != allocated_width || height != allocated_height || allocated_format != 1);
    if (size_changed) {
        allocated_width = width;
        allocated_height = height;
        allocated_format = 1;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // Upload Y Plane (GL_RED, width x height)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride);
    glBindTexture(GL_TEXTURE_2D, y_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, y_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, y_plane);
    }

    // Upload Interleaved UV Plane (GL_RG, width/2 x height/2)
    // Note: uv_stride is in bytes. Each pixel has 2 bytes (RG), so row length in pixels is uv_stride / 2
    glPixelStorei(GL_UNPACK_ROW_LENGTH, uv_stride / 2);
    glBindTexture(GL_TEXTURE_2D, uv_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, width / 2, height / 2, 0, GL_RG, GL_UNSIGNED_BYTE, uv_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RG, GL_UNSIGNED_BYTE, uv_plane);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    has_video_frame = true;
}

void ShaderRenderer::upload_p010_frame(uint8_t* y_plane, uint8_t* uv_plane,
                                       int y_stride, int uv_stride,
                                       int width, int height) {
    format_mode = 1; // P010 uses NV12 shader sampling path (with 16-bit textures)
    current_width = width;
    current_height = height;

    bool size_changed = (width != allocated_width || height != allocated_height || allocated_format != 3);
    if (size_changed) {
        allocated_width = width;
        allocated_height = height;
        allocated_format = 3;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);

    // Upload 16-bit Y Plane (GL_R16, width x height)
    // Note: y_stride is in bytes. Each pixel is 2 bytes (uint16), so pixel stride is y_stride / 2
    glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride / 2);
    glBindTexture(GL_TEXTURE_2D, y_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, width, height, 0, GL_RED, GL_UNSIGNED_SHORT, y_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_SHORT, y_plane);
    }

    // Upload 16-bit Interleaved UV Plane (GL_RG16, width/2 x height/2)
    // Note: uv_stride is in bytes. Each UV pair is 4 bytes (2x uint16), so RG row length is uv_stride / 4
    glPixelStorei(GL_UNPACK_ROW_LENGTH, uv_stride / 4);
    glBindTexture(GL_TEXTURE_2D, uv_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16, width / 2, height / 2, 0, GL_RG, GL_UNSIGNED_SHORT, uv_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RG, GL_UNSIGNED_SHORT, uv_plane);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    has_video_frame = true;
}

void ShaderRenderer::upload_yuv10_frame(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                        int y_stride, int u_stride, int v_stride,
                                        int width, int height) {
    format_mode = 3; // 10-bit Planar YUV (YUV420P10)
    current_width = width;
    current_height = height;

    bool size_changed = (width != allocated_width || height != allocated_height || allocated_format != 4);
    if (size_changed) {
        allocated_width = width;
        allocated_height = height;
        allocated_format = 4;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride / 2);
    glBindTexture(GL_TEXTURE_2D, y_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, width, height, 0, GL_RED, GL_UNSIGNED_SHORT, y_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_SHORT, y_plane);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, u_stride / 2);
    glBindTexture(GL_TEXTURE_2D, u_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_SHORT, u_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RED, GL_UNSIGNED_SHORT, u_plane);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, v_stride / 2);
    glBindTexture(GL_TEXTURE_2D, v_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_SHORT, v_plane);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RED, GL_UNSIGNED_SHORT, v_plane);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    has_video_frame = true;
}

void ShaderRenderer::upload_rgb_frame(uint8_t* rgb_data, int width, int height) {
    format_mode = 2; // RGB
    current_width = width;
    current_height = height;

    bool size_changed = (width != allocated_width || height != allocated_height || allocated_format != 2);
    if (size_changed) {
        allocated_width = width;
        allocated_height = height;
        allocated_format = 2;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, y_texture);
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_data);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb_data);
    }
    has_video_frame = true;
}

void ShaderRenderer::upload_thumbnail_frame(const uint8_t* rgb_data, int width, int height) {
    if (!rgb_data || width <= 0 || height <= 0) return;

    if (thumbnail_texture == 0) {
        glGenTextures(1, &thumbnail_texture);
        glBindTexture(GL_TEXTURE_2D, thumbnail_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, thumbnail_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_data);

    thumbnail_width = width;
    thumbnail_height = height;
    has_thumbnail_texture = true;
}

void ShaderRenderer::render(int window_width, int window_height, bool menu_bar_visible) {
    glViewport(0, 0, window_width, window_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!has_video_frame || current_width <= 0 || current_height <= 0) {
        return;
    }

    float menu_h = menu_bar_visible ? 26.0f : 0.0f;
    int video_viewport_h = std::max(1, window_height - static_cast<int>(menu_h));
    glViewport(0, 0, window_width, video_viewport_h);

    glUseProgram(shader_program);

    float scale_x = 1.0f;
    float scale_y = 1.0f;

    float avail_w = static_cast<float>(window_width);
    float avail_h = static_cast<float>(video_viewport_h);

    if (current_width > 0 && current_height > 0 && avail_w > 0.0f && avail_h > 0.0f) {
        if (render_mode == RenderMode::FIT) {
            float target_aspect = static_cast<float>(current_width) / current_height;
            if (aspect_ratio_mode == AspectRatioMode::RATIO_16_9) {
                target_aspect = 16.0f / 9.0f;
            } else if (aspect_ratio_mode == AspectRatioMode::RATIO_4_3) {
                target_aspect = 4.0f / 3.0f;
            } else if (aspect_ratio_mode == AspectRatioMode::RATIO_21_9) {
                target_aspect = 21.0f / 9.0f;
            } else if (aspect_ratio_mode == AspectRatioMode::RATIO_1_1) {
                target_aspect = 1.0f;
            } else if (aspect_ratio_mode == AspectRatioMode::RATIO_FILL) {
                target_aspect = avail_w / avail_h;
            }

            float window_aspect = avail_w / avail_h;
            if (target_aspect > window_aspect) {
                scale_x = 1.0f;
                scale_y = window_aspect / target_aspect;
            } else {
                scale_x = target_aspect / window_aspect;
                scale_y = 1.0f;
            }
        } else if (render_mode == RenderMode::PIXEL_PERFECT) {
            scale_x = static_cast<float>(current_width) / avail_w;
            scale_y = static_cast<float>(current_height) / avail_h;
        } else if (render_mode == RenderMode::STRETCH) {
            scale_x = 1.0f;
            scale_y = 1.0f;
        }
    }

    glUniform2f(glGetUniformLocation(shader_program, "uScale"), scale_x, scale_y);
    glUniform1f(glGetUniformLocation(shader_program, "uZoom"), zoom_scale);
    glUniform2f(glGetUniformLocation(shader_program, "uPan"), pan_offset_x, pan_offset_y);
    glUniform1i(glGetUniformLocation(shader_program, "formatMode"), format_mode);
    glUniform1i(glGetUniformLocation(shader_program, "enableHDR"), hdr_enabled ? 1 : 0);
    glUniform1i(glGetUniformLocation(shader_program, "enableSharpness"), sharpness_enabled ? 1 : 0);
    glUniform1f(glGetUniformLocation(shader_program, "sharpnessStrength"), sharpness_strength);
    glUniform2f(glGetUniformLocation(shader_program, "uTexSize"), static_cast<float>(current_width), static_cast<float>(current_height));
    glUniform1f(glGetUniformLocation(shader_program, "uBrightness"), brightness);
    glUniform1f(glGetUniformLocation(shader_program, "uContrast"), contrast);
    glUniform1f(glGetUniformLocation(shader_program, "uSaturation"), saturation);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, y_texture);
    glUniform1i(glGetUniformLocation(shader_program, "texY"), 0);

    if (format_mode == 0 || format_mode == 3) {
        // YUV420P / YUV420P10 (Planar YUV)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, u_texture);
        glUniform1i(glGetUniformLocation(shader_program, "texU"), 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, v_texture);
        glUniform1i(glGetUniformLocation(shader_program, "texV"), 2);
    } else if (format_mode == 1) {
        // NV12 / P010
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, uv_texture);
        glUniform1i(glGetUniformLocation(shader_program, "texUV"), 1);
    }

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Restore full window viewport so UI elements (menu bar, OSD, subtitles) render cleanly across entire window
    glViewport(0, 0, window_width, window_height);
}

void ShaderRenderer::draw_ui_rect(float x, float y, float w, float h, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h) {
    if (ui_alpha <= 0.001f || !ui_shader_program || w <= 0.0f || h <= 0.0f) return;
    float vertices[] = {
        x,     y,     0.0f, 0.0f,
        x,     y + h, 0.0f, 1.0f,
        x + w, y + h, 1.0f, 1.0f,

        x,     y,     0.0f, 0.0f,
        x + w, y + h, 1.0f, 1.0f,
        x + w, y,     1.0f, 0.0f
    };

    glUseProgram(ui_shader_program);
    glUniform2f(glGetUniformLocation(ui_shader_program, "uScreenSize"), static_cast<float>(win_w), static_cast<float>(win_h));
    glUniform1i(glGetUniformLocation(ui_shader_program, "uUseTex"), 0);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsCircle"), 0);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsRoundedRect"), 0);
    glUniform4f(glGetUniformLocation(ui_shader_program, "uColor"), r, g, b, a);
    glUniform1f(glGetUniformLocation(ui_shader_program, "uAlpha"), ui_alpha);

    glBindVertexArray(ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}

void ShaderRenderer::draw_ui_rounded_rect(float x, float y, float w, float h, float radius, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h) {
    if (ui_alpha <= 0.001f || !ui_shader_program || w <= 0.0f || h <= 0.0f) return;
    float vertices[] = {
        x,     y,     0.0f, 0.0f,
        x,     y + h, 0.0f, 1.0f,
        x + w, y + h, 1.0f, 1.0f,

        x,     y,     0.0f, 0.0f,
        x + w, y + h, 1.0f, 1.0f,
        x + w, y,     1.0f, 0.0f
    };

    glUseProgram(ui_shader_program);
    glUniform2f(glGetUniformLocation(ui_shader_program, "uScreenSize"), static_cast<float>(win_w), static_cast<float>(win_h));
    glUniform1i(glGetUniformLocation(ui_shader_program, "uUseTex"), 0);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsCircle"), 0);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsRoundedRect"), 1);
    glUniform2f(glGetUniformLocation(ui_shader_program, "uRectSize"), w, h);
    glUniform1f(glGetUniformLocation(ui_shader_program, "uRadius"), radius);
    glUniform4f(glGetUniformLocation(ui_shader_program, "uColor"), r, g, b, a);
    glUniform1f(glGetUniformLocation(ui_shader_program, "uAlpha"), ui_alpha);

    glBindVertexArray(ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}

void ShaderRenderer::draw_ui_circle(float cx, float cy, float radius, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h) {
    if (ui_alpha <= 0.001f || !ui_shader_program || radius <= 0.0f) return;
    float x = cx - radius;
    float y = cy - radius;
    float w = radius * 2.0f;
    float h = radius * 2.0f;

    float vertices[] = {
        x,     y,     0.0f, 0.0f,
        x,     y + h, 0.0f, 1.0f,
        x + w, y + h, 1.0f, 1.0f,

        x,     y,     0.0f, 0.0f,
        x + w, y + h, 1.0f, 1.0f,
        x + w, y,     1.0f, 0.0f
    };

    glUseProgram(ui_shader_program);
    glUniform2f(glGetUniformLocation(ui_shader_program, "uScreenSize"), static_cast<float>(win_w), static_cast<float>(win_h));
    glUniform1i(glGetUniformLocation(ui_shader_program, "uUseTex"), 0);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsCircle"), 1);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsRoundedRect"), 0);
    glUniform4f(glGetUniformLocation(ui_shader_program, "uColor"), r, g, b, a);
    glUniform1f(glGetUniformLocation(ui_shader_program, "uAlpha"), ui_alpha);

    glBindVertexArray(ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}

void ShaderRenderer::draw_ui_icon(GLuint tex, float x, float y, float w, float h, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h) {
    if (ui_alpha <= 0.001f || !ui_shader_program || !tex || w <= 0.0f || h <= 0.0f) return;
    float vertices[] = {
        x,     y,     0.0f, 0.0f,
        x,     y + h, 0.0f, 1.0f,
        x + w, y + h, 1.0f, 1.0f,

        x,     y,     0.0f, 0.0f,
        x + w, y + h, 1.0f, 1.0f,
        x + w, y,     1.0f, 0.0f
    };

    glUseProgram(ui_shader_program);
    glUniform2f(glGetUniformLocation(ui_shader_program, "uScreenSize"), static_cast<float>(win_w), static_cast<float>(win_h));
    glUniform1i(glGetUniformLocation(ui_shader_program, "uUseTex"), 1);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsCircle"), 0);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsRoundedRect"), 0);
    glUniform4f(glGetUniformLocation(ui_shader_program, "uColor"), r, g, b, a);
    glUniform1f(glGetUniformLocation(ui_shader_program, "uAlpha"), ui_alpha);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uTex"), 0);

    glBindVertexArray(ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}

bool ShaderRenderer::init_font_engine() {
    if (font_initialized) return true;

    FT_Library ft = nullptr;
    if (FT_Init_FreeType(&ft)) {
        std::cerr << "[VMP Font] Failed to initialize FreeType library!" << std::endl;
        return false;
    }
    ft_library = ft;

    FT_Face face = nullptr;
    FT_Error err = FT_New_Memory_Face(ft, EMBEDDED_FONT_GOOGLE_SANS, EMBEDDED_FONT_SIZE, 0, &face);
    if (err) {
        std::cerr << "[VMP Font] Failed to load embedded Google Sans font, error: " << err << std::endl;
    } else {
        ft_face = face;
        FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    }

    const char* fallback_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf"
    };
    for (const char* path : fallback_paths) {
        FT_Face fb = nullptr;
        if (FT_New_Face(ft, path, 0, &fb) == 0) {
            ft_fallback_face = fb;
            FT_Select_Charmap(fb, FT_ENCODING_UNICODE);
            break;
        }
    }

    font_initialized = (ft_face != nullptr || ft_fallback_face != nullptr);
    return font_initialized;
}

void ShaderRenderer::cleanup_font_engine() {
    for (auto& pair : glyph_cache) {
        if (pair.second.texture) {
            glDeleteTextures(1, &pair.second.texture);
        }
    }
    glyph_cache.clear();

    if (ft_face) {
        FT_Done_Face(static_cast<FT_Face>(ft_face));
        ft_face = nullptr;
    }
    if (ft_fallback_face) {
        FT_Done_Face(static_cast<FT_Face>(ft_fallback_face));
        ft_fallback_face = nullptr;
    }
    if (ft_library) {
        FT_Done_FreeType(static_cast<FT_Library>(ft_library));
        ft_library = nullptr;
    }
    font_initialized = false;
}

static std::vector<uint32_t> process_text_to_codepoints(const std::string& text) {
    std::vector<uint32_t> result;
    if (text.empty()) return result;

    bool has_non_ascii = false;
    for (char c : text) {
        if (static_cast<unsigned char>(c) >= 128) {
            has_non_ascii = true;
            break;
        }
    }

    if (!has_non_ascii) {
        result.reserve(text.size());
        for (char c : text) {
            result.push_back(static_cast<unsigned char>(c));
        }
        return result;
    }

    std::vector<FriBidiChar> unicode(text.size() * 2 + 16);
    FriBidiStrIndex len = fribidi_charset_to_unicode(FRIBIDI_CHAR_SET_UTF8, text.c_str(), text.length(), unicode.data());
    if (len <= 0) return result;

    std::vector<FriBidiCharType> bidi_types(len);
    fribidi_get_bidi_types(unicode.data(), len, bidi_types.data());

    std::vector<FriBidiBracketType> bracket_types(len);
    fribidi_get_bracket_types(unicode.data(), len, bidi_types.data(), bracket_types.data());

    FriBidiParType base_dir = FRIBIDI_PAR_ON;
    std::vector<FriBidiLevel> levels(len);
    FriBidiLevel lvl1 = fribidi_get_par_embedding_levels_ex(bidi_types.data(), bracket_types.data(), len, &base_dir, levels.data());
    (void)lvl1;

    std::vector<FriBidiJoiningType> jtypes(len);
    fribidi_get_joining_types(unicode.data(), len, jtypes.data());
    fribidi_join_arabic(bidi_types.data(), len, levels.data(), jtypes.data());
    fribidi_shape(FRIBIDI_FLAGS_DEFAULT | FRIBIDI_FLAGS_ARABIC, levels.data(), len, jtypes.data(), unicode.data());
    FriBidiLevel lvl2 = fribidi_reorder_line(FRIBIDI_FLAGS_DEFAULT, bidi_types.data(), len, 0, base_dir, levels.data(), unicode.data(), nullptr);
    (void)lvl2;

    result.assign(unicode.begin(), unicode.begin() + len);
    return result;
}

const ShaderRenderer::GlyphTexture* ShaderRenderer::get_glyph(uint32_t codepoint, int font_size) {
    if (!font_initialized && !init_font_engine()) return nullptr;

    uint64_t key = (static_cast<uint64_t>(font_size) << 32) | static_cast<uint64_t>(codepoint);
    auto it = glyph_cache.find(key);
    if (it != glyph_cache.end()) {
        return &it->second;
    }

    FT_Face face = static_cast<FT_Face>(ft_face);
    FT_Face fallback = static_cast<FT_Face>(ft_fallback_face);
    FT_Face selected_face = nullptr;

    FT_UInt glyph_idx = 0;
    if (face) {
        glyph_idx = FT_Get_Char_Index(face, codepoint);
        if (glyph_idx != 0) {
            selected_face = face;
        }
    }
    if (glyph_idx == 0 && fallback) {
        glyph_idx = FT_Get_Char_Index(fallback, codepoint);
        if (glyph_idx != 0) {
            selected_face = fallback;
        }
    }
    if (!selected_face) {
        selected_face = (face) ? face : fallback;
        if (!selected_face) return nullptr;
        glyph_idx = 0;
    }

    FT_Set_Pixel_Sizes(selected_face, 0, static_cast<FT_UInt>(font_size));
    if (FT_Load_Glyph(selected_face, glyph_idx, FT_LOAD_RENDER)) {
        return nullptr;
    }

    FT_GlyphSlot slot = selected_face->glyph;
    GlyphTexture gt;
    gt.width = slot->bitmap.width;
    gt.height = slot->bitmap.rows;
    gt.bearing_x = slot->bitmap_left;
    gt.bearing_y = slot->bitmap_top;
    gt.advance = static_cast<float>(slot->advance.x >> 6);

    if (gt.width > 0 && gt.height > 0 && slot->bitmap.buffer) {
        std::vector<uint8_t> rgba(gt.width * gt.height * 4);
        for (int i = 0; i < gt.width * gt.height; ++i) {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = slot->bitmap.buffer[i];
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glGenTextures(1, &gt.texture);
        glBindTexture(GL_TEXTURE_2D, gt.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gt.width, gt.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    auto inserted = glyph_cache.emplace(key, gt);
    return &inserted.first->second;
}

float ShaderRenderer::get_text_width(const std::string& text, float font_size) {
    if (text.empty()) return 0.0f;
    int sz = static_cast<int>(std::round(font_size));
    if (sz < 8) sz = 8;

    auto codepoints = process_text_to_codepoints(text);
    float total_w = 0.0f;
    for (uint32_t cp : codepoints) {
        const GlyphTexture* gt = get_glyph(cp, sz);
        if (gt) {
            total_w += gt->advance;
        } else {
            total_w += font_size * 0.5f;
        }
    }
    return total_w;
}

void ShaderRenderer::draw_ui_text(const std::string& text, float x, float y, float font_size, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h) {
    if (text.empty() || ui_alpha <= 0.001f || !ui_shader_program) return;
    int sz = static_cast<int>(std::round(font_size));
    if (sz < 8) sz = 8;

    auto codepoints = process_text_to_codepoints(text);
    if (codepoints.empty()) return;

    float baseline_y = y + static_cast<float>(sz) * 0.78f;
    float cur_x = x;

    glUseProgram(ui_shader_program);
    glUniform2f(glGetUniformLocation(ui_shader_program, "uScreenSize"), static_cast<float>(win_w), static_cast<float>(win_h));
    glUniform1i(glGetUniformLocation(ui_shader_program, "uUseTex"), 1);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsCircle"), 0);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uIsRoundedRect"), 0);
    glUniform4f(glGetUniformLocation(ui_shader_program, "uColor"), r, g, b, a);
    glUniform1f(glGetUniformLocation(ui_shader_program, "uAlpha"), ui_alpha);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(ui_shader_program, "uTex"), 0);

    glBindVertexArray(ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (uint32_t cp : codepoints) {
        const GlyphTexture* gt = get_glyph(cp, sz);
        if (gt) {
            if (gt->texture && gt->width > 0 && gt->height > 0) {
                float gx = cur_x + static_cast<float>(gt->bearing_x);
                float gy = baseline_y - static_cast<float>(gt->bearing_y);
                float gw = static_cast<float>(gt->width);
                float gh = static_cast<float>(gt->height);

                float vertices[] = {
                    gx,      gy,      0.0f, 0.0f,
                    gx,      gy + gh, 0.0f, 1.0f,
                    gx + gw, gy + gh, 1.0f, 1.0f,

                    gx,      gy,      0.0f, 0.0f,
                    gx + gw, gy + gh, 1.0f, 1.0f,
                    gx + gw, gy,      1.0f, 0.0f
                };

                glBindTexture(GL_TEXTURE_2D, gt->texture);
                glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            cur_x += gt->advance;
        } else {
            cur_x += font_size * 0.5f;
        }
    }

    glDisable(GL_BLEND);
}

void ShaderRenderer::draw_ui_string(const std::string& text, float x, float y, float char_w, float char_h, float r, float g, float b, float a, float ui_alpha, int win_w, int win_h) {
    (void)char_w;
    float font_sz = (char_h > 0.0f) ? char_h : 14.0f;
    draw_ui_text(text, x, y, font_sz, r, g, b, a, ui_alpha, win_w, win_h);
}

void ShaderRenderer::render_ui_overlay(int win_w, int win_h, double current_sec, double total_sec, 
                                       bool is_paused, bool is_fullscreen, float ui_alpha, double mouse_x, double mouse_y, 
                                       bool is_scrubbing, bool show_stats, const std::string& video_name, 
                                       const std::string& video_size, const std::string& video_date, 
                                       const std::string& video_resolution, const std::string& codec_name, 
                                       double fps, const std::string& hw_status, const std::string& active_subtitle,
                                       const std::string& osd_notification) {
    if (win_w <= 0 || win_h <= 0) return;
    glViewport(0, 0, win_w, win_h);

    // 0. Render OSD Notification Pill (always visible, does not fade with OSD)
    if (!osd_notification.empty()) {
        float note_font_sz = 14.0f;
        float note_y = is_fullscreen ? 16.0f : 36.0f;
        float note_x = 20.0f; // 20px from the left of the window

        float note_w = get_text_width(osd_notification, note_font_sz);

        float bg_padding = 12.0f;
        float note_h = note_font_sz + 8.0f;
        float pill_radius = note_h / 2.0f;
        
        // Translucent cyan tinted capsule background
        draw_ui_rounded_rect(note_x - bg_padding, note_y - 4.0f, note_w + bg_padding * 2.0f, note_h, 
                             pill_radius, 0.0f, 0.08f, 0.16f, 0.92f, 1.0f, win_w, win_h);

        draw_ui_text(osd_notification, note_x, note_y, note_font_sz, 
                     1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);
    }

    // 0.1. Render Subtitles (always visible, does not fade with OSD)
    if (!active_subtitle.empty()) {
        float sub_font_sz = 18.0f;
        float sub_text_w = get_text_width(active_subtitle, sub_font_sz);
        
        float sub_x = (win_w - sub_text_w) / 2.0f;
        float sub_y = win_h - 100.0f; // 100 pixels from the bottom of the window

        float bg_padding = 12.0f;
        float sub_h = sub_font_sz + 8.0f;
        draw_ui_rounded_rect(sub_x - bg_padding, sub_y - 4.0f, sub_text_w + bg_padding * 2.0f, sub_h, 
                             6.0f, 0.0f, 0.0f, 0.0f, 0.65f, 1.0f, win_w, win_h);

        draw_ui_text(active_subtitle, sub_x, sub_y, sub_font_sz, 
                     1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);
    }

    if (ui_alpha <= 0.01f) return;

    float center_y = win_h - 40.0f;
    float icon_size = 22.0f;

    // 1. Bottom Control Bar Icons (Far Left: Replay 5s, Play/Pause, Forward 5s)
    float replay_x = 24.0f;
    float play_x = 58.0f;
    float forward_x = 92.0f;
    float icon_y = center_y - (icon_size / 2.0f);

    draw_ui_icon(tex_icon_replay5, replay_x, icon_y, icon_size, icon_size, 1.0f, 1.0f, 1.0f, 1.0f, ui_alpha, win_w, win_h);
    GLuint play_pause_tex = is_paused ? tex_icon_play : tex_icon_pause;
    draw_ui_icon(play_pause_tex, play_x, icon_y, icon_size, icon_size, 1.0f, 1.0f, 1.0f, 1.0f, ui_alpha, win_w, win_h);
    draw_ui_icon(tex_icon_forward5, forward_x, icon_y, icon_size, icon_size, 1.0f, 1.0f, 1.0f, 1.0f, ui_alpha, win_w, win_h);

    // 2. Time Display Badge (Next to Forward button)
    bool has_hours = (total_sec >= 3600.0);
    std::string full_time = fmt_time_short(current_sec, has_hours) + " / " + fmt_time_short(total_sec, has_hours);
    float time_x = forward_x + icon_size + 14.0f;
    float time_y = center_y - 7.0f;
    draw_ui_text(full_time, time_x, time_y, 14.0f, 0.9f, 0.95f, 1.0f, 0.95f, ui_alpha, win_w, win_h);

    // Calculate progress bar start AFTER time display and end BEFORE fullscreen button
    float time_text_w = get_text_width(full_time, 14.0f);
    float bar_x = time_x + time_text_w + 15.0f;
    float fs_x = win_w - 42.0f;
    float fs_y = center_y - (icon_size / 2.0f);
    float bar_w = (fs_x - 15.0f) - bar_x;
    float bar_y = center_y;

    bool is_hover = (mouse_x >= bar_x - 10.0f && mouse_x <= bar_x + bar_w + 10.0f &&
                    mouse_y >= bar_y - 20.0f && mouse_y <= bar_y + 20.0f);

    float bar_h = (is_hover || is_scrubbing) ? 8.0f : 5.0f;
    float bar_pos_y = bar_y - (bar_h / 2.0f);
    float track_radius = bar_h / 2.0f;

    // 3. Progress Bar Track (Capsule rounded ends)
    draw_ui_rounded_rect(bar_x, bar_pos_y, bar_w, bar_h, track_radius, 1.0f, 1.0f, 1.0f, 0.25f, ui_alpha, win_w, win_h);

    // 4. Progress Bar Fill (Vibrant Cyan / Electric Blue Capsule)
    double progress_ratio = (total_sec > 0.0) ? std::max(0.0, std::min(1.0, current_sec / total_sec)) : 0.0;
    float fill_w = static_cast<float>(bar_w * progress_ratio);
    if (fill_w > 0.0f) {
        float fill_radius = std::min(track_radius, fill_w / 2.0f);
        draw_ui_rounded_rect(bar_x, bar_pos_y, fill_w, bar_h, fill_radius, 0.0f, 0.85f, 1.0f, 0.95f, ui_alpha, win_w, win_h);
    }

    // 5. Playback Knob (Glowing Circular Handle)
    float knob_radius = (is_hover || is_scrubbing) ? 9.0f : 7.0f;
    float knob_cx = bar_x + fill_w;
    float knob_cy = bar_y;

    // Glowing outer halo circle
    draw_ui_circle(knob_cx, knob_cy, knob_radius + 4.0f, 0.0f, 0.85f, 1.0f, 0.35f, ui_alpha, win_w, win_h);
    // Solid white circle handle
    draw_ui_circle(knob_cx, knob_cy, knob_radius, 1.0f, 1.0f, 1.0f, 1.0f, ui_alpha, win_w, win_h);
    // Inner vibrant cyan dot
    draw_ui_circle(knob_cx, knob_cy, knob_radius * 0.45f, 0.0f, 0.85f, 1.0f, 1.0f, ui_alpha, win_w, win_h);

    // 7. Hover Guide & Timestamp Tooltip Box (with optional Video Thumbnail Preview)
    if ((is_hover || is_scrubbing) && total_sec > 0.0) {
        double hover_ratio = std::max(0.0, std::min(1.0, (mouse_x - bar_x) / bar_w));
        double hover_sec = hover_ratio * total_sec;
        float h_x = bar_x + static_cast<float>(bar_w * hover_ratio);

        // Hover Vertical Guide line
        draw_ui_rect(h_x - 1.0f, bar_pos_y - 4.0f, 2.0f, bar_h + 8.0f, 1.0f, 1.0f, 1.0f, 0.5f, ui_alpha, win_w, win_h);

        std::string time_tip = fmt_time_short(hover_sec, has_hours);
        float pill_w = has_hours ? 84.0f : 64.0f;
        float pill_h = 24.0f;
        float pill_rad = pill_h / 2.0f;

        if (has_thumbnail_texture && thumbnail_texture != 0) {
            float tw = 160.0f;
            float th = 90.0f;
            float tx = std::max(bar_x, std::min(bar_x + bar_w - tw, h_x - (tw / 2.0f)));
            float ty = bar_pos_y - (th + 38.0f);

            // Semi-translucent dark background with uniform rounded border
            draw_ui_rounded_rect(tx - 3.0f, ty - 3.0f, tw + 6.0f, th + 6.0f, 6.0f, 0.0f, 0.85f, 1.0f, 0.30f, ui_alpha, win_w, win_h);
            draw_ui_rounded_rect(tx - 2.0f, ty - 2.0f, tw + 4.0f, th + 4.0f, 5.0f, 0.03f, 0.06f, 0.12f, 0.95f, ui_alpha, win_w, win_h);

            // Thumbnail Image
            draw_ui_icon(thumbnail_texture, tx, ty, tw, th, 1.0f, 1.0f, 1.0f, 1.0f, ui_alpha, win_w, win_h);

            // Hover timestamp capsule below thumbnail image
            float pill_x = tx + (tw - pill_w) / 2.0f;
            float pill_y = ty + th + 4.0f;

            draw_ui_rounded_rect(pill_x, pill_y, pill_w, pill_h, pill_rad, 0.05f, 0.08f, 0.16f, 0.95f, ui_alpha, win_w, win_h);
            draw_ui_string(time_tip, pill_x + 8.0f, pill_y + 4.0f, 11.0f, 14.0f, 1.0f, 1.0f, 1.0f, 1.0f, ui_alpha, win_w, win_h);
        } else {
            // Standard floating timestamp tooltip capsule
            float pill_x = std::max(bar_x, std::min(bar_x + bar_w - pill_w, h_x - (pill_w / 2.0f)));
            float pill_y = bar_pos_y - 42.0f;

            draw_ui_rounded_rect(pill_x, pill_y, pill_w, pill_h, pill_rad, 0.05f, 0.08f, 0.16f, 0.95f, ui_alpha, win_w, win_h);
            draw_ui_string(time_tip, pill_x + 8.0f, pill_y + 4.0f, 11.0f, 14.0f, 1.0f, 1.0f, 1.0f, 1.0f, ui_alpha, win_w, win_h);
        }
    }

    // 8. Fullscreen Icon (Bottom Right)
    GLuint fs_tex = is_fullscreen ? tex_icon_fullscreen_exit : tex_icon_fullscreen;
    draw_ui_icon(fs_tex, fs_x, fs_y, icon_size, icon_size, 1.0f, 1.0f, 1.0f, 1.0f, ui_alpha, win_w, win_h);

    // 9. Top-Right "BACK" Button & Stats (Render ONLY in windowed mode, 100% clean in fullscreen like VLC)
    if (!is_fullscreen) {
        float back_btn_w = 110.0f;
        float back_btn_h = 32.0f;
        float back_btn_x = win_w - back_btn_w - 20.0f;
        float back_btn_y = 36.0f;

        bool back_hover = (mouse_x >= back_btn_x && mouse_x <= back_btn_x + back_btn_w &&
                          mouse_y >= back_btn_y && mouse_y <= back_btn_y + back_btn_h);

        if (back_hover) {
            draw_ui_rounded_rect(back_btn_x, back_btn_y, back_btn_w, back_btn_h, 16.0f, 0.0f, 0.85f, 1.0f, 0.90f, ui_alpha, win_w, win_h);
            draw_ui_string("< BACK", back_btn_x + 22.0f, back_btn_y + 8.0f, 10.0f, 14.0f, 0.02f, 0.05f, 0.10f, 1.0f, ui_alpha, win_w, win_h);
        } else {
            draw_ui_rounded_rect(back_btn_x, back_btn_y, back_btn_w, back_btn_h, 16.0f, 0.0f, 0.40f, 0.70f, 0.45f, ui_alpha, win_w, win_h);
            draw_ui_string("< BACK", back_btn_x + 22.0f, back_btn_y + 8.0f, 10.0f, 14.0f, 1.0f, 1.0f, 1.0f, 0.95f, ui_alpha, win_w, win_h);
        }

        // 10. Statistics Icon (Top Right, to the left of BACK button)
        float stats_x = back_btn_x - 38.0f;
        float stats_y = back_btn_y + 5.0f;

        if (show_stats) {
            draw_ui_icon(tex_icon_stats, stats_x, stats_y, icon_size, icon_size, 0.0f, 0.85f, 1.0f, 1.0f, ui_alpha, win_w, win_h);
        } else {
            draw_ui_icon(tex_icon_stats, stats_x, stats_y, icon_size, icon_size, 1.0f, 1.0f, 1.0f, 1.0f, ui_alpha, win_w, win_h);
        }
    }

    // 10. Center Screen Pause Badge Overlay
    if (is_paused) {
        float center_sz = 48.0f;
        float center_x = (win_w - center_sz) / 2.0f;
        float center_y_screen = (win_h - center_sz) / 2.0f;
        draw_ui_icon(tex_icon_pause, center_x, center_y_screen, center_sz, center_sz, 1.0f, 1.0f, 1.0f, 0.95f, ui_alpha, win_w, win_h);
    }

    // 11. Statistics Info Popup Window (Top Right below details/stats icon)
    if (show_stats) {
        float pop_w = 420.0f;
        float pop_h = 240.0f;
        float pop_x = std::max(20.0f, win_w - pop_w - 20.0f);
        float pop_y = is_fullscreen ? 20.0f : (36.0f + 5.0f + icon_size + 10.0f);

        // Uniform 1px subtle glass/cyan border around the entire rounded card
        draw_ui_rounded_rect(pop_x, pop_y, pop_w, pop_h, 8.0f, 0.0f, 0.85f, 1.0f, 0.30f, ui_alpha, win_w, win_h);

        // Dark translucent card inner fill
        draw_ui_rounded_rect(pop_x + 1.0f, pop_y + 1.0f, pop_w - 2.0f, pop_h - 2.0f, 7.0f, 0.04f, 0.07f, 0.14f, 0.94f, ui_alpha, win_w, win_h);

        // Text content
        float text_x = pop_x + 15.0f;
        float text_y = pop_y + 15.0f;
        float line_h = 18.0f;

        draw_ui_string("VIDEO TELEMETRY & STATS", text_x, text_y, 11.0f, 14.0f, 0.0f, 0.85f, 1.0f, 1.0f, ui_alpha, win_w, win_h);
        text_y += line_h + 5.0f;

        draw_ui_string("File: " + video_name, text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h); text_y += line_h;
        draw_ui_string("Size: " + video_size, text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h); text_y += line_h;
        draw_ui_string("Date: " + video_date, text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h); text_y += line_h;
        draw_ui_string("Resolution: " + video_resolution, text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h); text_y += line_h;
        draw_ui_string("Codec: " + codec_name, text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h); text_y += line_h;
        draw_ui_string("FPS: " + std::to_string(static_cast<int>(fps)), text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h); text_y += line_h;
        draw_ui_string("HW Accel: " + hw_status, text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h); text_y += line_h;
        std::string cas_str = sharpness_enabled ? "ENABLED (0.65)" : "DISABLED";
        draw_ui_string("CAS Clarity: " + cas_str, text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h); text_y += line_h;
        draw_ui_string("Aspect Ratio: " + get_aspect_ratio_name(), text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h); text_y += line_h;
        
        std::string time_str = fmt_time_short(current_sec, has_hours) + " / " + fmt_time_short(total_sec, has_hours);
        draw_ui_string("Playback: " + time_str, text_x, text_y, 10.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.9f, ui_alpha, win_w, win_h);
    }
}

bool ShaderRenderer::save_framebuffer_screenshot(int width, int height, const std::string& filepath) {
    if (width <= 0 || height <= 0) return false;

    int row_stride = ((width * 3 + 3) / 4) * 4; // 4-byte aligned stride for BMP
    std::vector<uint8_t> pixels(row_stride * height, 0);

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, pixels.data());

    std::ofstream out(filepath, std::ios::binary);
    if (!out.is_open()) return false;

    // BMP File Header (14 bytes)
    uint32_t file_size = 54 + static_cast<uint32_t>(pixels.size());
    uint32_t data_offset = 54;
    uint8_t file_header[14] = {
        'B', 'M',
        static_cast<uint8_t>(file_size & 0xFF), static_cast<uint8_t>((file_size >> 8) & 0xFF),
        static_cast<uint8_t>((file_size >> 16) & 0xFF), static_cast<uint8_t>((file_size >> 24) & 0xFF),
        0, 0, 0, 0,
        static_cast<uint8_t>(data_offset & 0xFF), static_cast<uint8_t>((data_offset >> 8) & 0xFF),
        static_cast<uint8_t>((data_offset >> 16) & 0xFF), static_cast<uint8_t>((data_offset >> 24) & 0xFF)
    };

    // DIB Header (40 bytes)
    uint32_t dib_size = 40;
    uint16_t planes = 1;
    uint16_t bpp = 24;
    uint32_t img_size = static_cast<uint32_t>(pixels.size());
    uint8_t dib_header[40] = {
        static_cast<uint8_t>(dib_size & 0xFF), static_cast<uint8_t>((dib_size >> 8) & 0xFF),
        static_cast<uint8_t>((dib_size >> 16) & 0xFF), static_cast<uint8_t>((dib_size >> 24) & 0xFF),
        static_cast<uint8_t>(width & 0xFF), static_cast<uint8_t>((width >> 8) & 0xFF),
        static_cast<uint8_t>((width >> 16) & 0xFF), static_cast<uint8_t>((width >> 24) & 0xFF),
        static_cast<uint8_t>(height & 0xFF), static_cast<uint8_t>((height >> 8) & 0xFF),
        static_cast<uint8_t>((height >> 16) & 0xFF), static_cast<uint8_t>((height >> 24) & 0xFF),
        static_cast<uint8_t>(planes & 0xFF), static_cast<uint8_t>((planes >> 8) & 0xFF),
        static_cast<uint8_t>(bpp & 0xFF), static_cast<uint8_t>((bpp >> 8) & 0xFF),
        0, 0, 0, 0, // No compression (BI_RGB)
        static_cast<uint8_t>(img_size & 0xFF), static_cast<uint8_t>((img_size >> 8) & 0xFF),
        static_cast<uint8_t>((img_size >> 16) & 0xFF), static_cast<uint8_t>((img_size >> 24) & 0xFF),
        0x13, 0x0B, 0, 0, // 2835 ppm print resolution
        0x13, 0x0B, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };

    out.write(reinterpret_cast<const char*>(file_header), sizeof(file_header));
    out.write(reinterpret_cast<const char*>(dib_header), sizeof(dib_header));
    out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    out.close();

    return true;
}

void ShaderRenderer::render_welcome_screen(int win_w, int win_h, double mouse_x, double mouse_y) {
    if (win_w <= 0 || win_h <= 0) return;
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.035f, 0.045f, 0.075f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Responsive scaling factor based on display resolution (scaled comfortably for fullscreen/HiDPI)
    float scale = std::clamp(std::min(static_cast<float>(win_w) / 1280.0f, static_cast<float>(win_h) / 720.0f), 0.85f, 2.0f);

    // Central Drag & Drop Card Dimensions
    float card_w = std::min(static_cast<float>(win_w) - 40.0f, 680.0f * scale);
    float card_h = std::min(static_cast<float>(win_h) - 40.0f, 380.0f * scale);
    float card_x = (static_cast<float>(win_w) - card_w) / 2.0f;
    float card_y = (static_cast<float>(win_h) - card_h) / 2.0f;

    // Main Card background (clean VLC dark card with 1px border, NO hover halo)
    draw_ui_rounded_rect(card_x - 1.0f, card_y - 1.0f, card_w + 2.0f, card_h + 2.0f, 
                         17.0f * scale, 0.18f, 0.20f, 0.24f, 0.95f, 1.0f, win_w, win_h);
    draw_ui_rounded_rect(card_x, card_y, card_w, card_h, 
                         16.0f * scale, 0.08f, 0.09f, 0.12f, 0.98f, 1.0f, win_w, win_h);

    // Inner subtle border
    draw_ui_rounded_rect(card_x + 8.0f * scale, card_y + 8.0f * scale, 
                         card_w - 16.0f * scale, card_h - 16.0f * scale, 
                         12.0f * scale, 0.16f, 0.20f, 0.26f, 0.35f, 1.0f, win_w, win_h);

    // Central Icon (clean media emblem, no glowing halo)
    float icon_sz = 64.0f * scale;
    float icon_cx = card_x + (card_w - icon_sz) / 2.0f;
    float icon_cy = card_y + (35.0f * scale);

    draw_ui_circle(icon_cx + icon_sz / 2.0f, icon_cy + icon_sz / 2.0f, 40.0f * scale, 0.12f, 0.14f, 0.18f, 0.90f, 1.0f, win_w, win_h);
    draw_ui_icon(tex_icon_play, icon_cx, icon_cy, icon_sz, icon_sz, 0.0f, 0.70f, 0.95f, 0.90f, 1.0f, win_w, win_h);

    // Main Title
    std::string title_str = "VMP - VIDEO MAX PLAYER";
    float title_font_sz = 22.0f * scale;
    float title_w = get_text_width(title_str, title_font_sz);
    float title_x = card_x + (card_w - title_w) / 2.0f;
    float title_y = icon_cy + icon_sz + (22.0f * scale);
    draw_ui_text(title_str, title_x, title_y, title_font_sz, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);

    // Action Subtitle
    std::string action_str = "DROP YOUR FOLDER OR VIDEO HERE";
    float act_font_sz = 15.0f * scale;
    float act_w = get_text_width(action_str, act_font_sz);
    float act_x = card_x + (card_w - act_w) / 2.0f;
    float act_y = title_y + (32.0f * scale);
    draw_ui_text(action_str, act_x, act_y, act_font_sz, 0.0f, 0.88f, 1.0f, 0.95f, 1.0f, win_w, win_h);

    // Supported Formats Badge
    std::string fmt_str = "MP4  MKV  WEBM  AVI  MOV  FLV  TS  WMV";
    float fmt_font_sz = 12.0f * scale;
    float fmt_w = get_text_width(fmt_str, fmt_font_sz);
    float fmt_x = card_x + (card_w - fmt_w) / 2.0f;
    float fmt_y = act_y + (24.0f * scale);
    draw_ui_text(fmt_str, fmt_x, fmt_y, fmt_font_sz, 0.65f, 0.75f, 0.85f, 0.70f, 1.0f, win_w, win_h);

    // Interactive "OPEN FOLDER" Button
    float btn_w = 200.0f * scale;
    float btn_h = 36.0f * scale;
    float btn_x = card_x + (card_w - btn_w) / 2.0f;
    float btn_y = card_y + card_h - (60.0f * scale);

    bool btn_hover = (mouse_x >= btn_x && mouse_x <= btn_x + btn_w &&
                      mouse_y >= btn_y && mouse_y <= btn_y + btn_h);

    std::string btn_txt = "OPEN FOLDER (Ctrl+O)";
    float txt_font_sz = 13.5f * scale;
    float txt_w = get_text_width(btn_txt, txt_font_sz);
    float txt_x = btn_x + (btn_w - txt_w) / 2.0f;

    if (btn_hover) {
        draw_ui_rounded_rect(btn_x, btn_y, btn_w, btn_h, btn_h / 2.0f, 0.0f, 0.50f, 0.85f, 0.90f, 1.0f, win_w, win_h);
        draw_ui_text(btn_txt, txt_x, btn_y + (10.0f * scale), txt_font_sz, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);
    } else {
        draw_ui_rounded_rect(btn_x, btn_y, btn_w, btn_h, btn_h / 2.0f, 0.16f, 0.20f, 0.28f, 0.85f, 1.0f, win_w, win_h);
        draw_ui_text(btn_txt, txt_x, btn_y + (10.0f * scale), txt_font_sz, 0.90f, 0.92f, 0.95f, 0.95f, 1.0f, win_w, win_h);
    }
}

GLuint ShaderRenderer::create_rgb_texture(const uint8_t* rgb_data, int width, int height) {
    if (!rgb_data || width <= 0 || height <= 0) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_data);
    return tex;
}

void ShaderRenderer::render_folder_gallery(int win_w, int win_h, double mouse_x, double mouse_y,
                                           const std::string& folder_path, const std::vector<FolderMediaItem>& items,
                                           int scroll_offset, bool is_fullscreen) {
    if (win_w <= 0 || win_h <= 0) return;
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.035f, 0.045f, 0.075f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 1. Header Bar (shifted below VLC menu bar if not fullscreen)
    float menu_bar_h = is_fullscreen ? 0.0f : 28.0f;
    float header_y = menu_bar_h;
    float header_h = 56.0f;
    draw_ui_rect(0, header_y, win_w, header_h, 0.05f, 0.07f, 0.12f, 0.95f, 1.0f, win_w, win_h);
    draw_ui_rect(0, header_y + header_h - 1.0f, win_w, 1.0f, 0.0f, 0.85f, 1.0f, 0.35f, 1.0f, win_w, win_h);

    // Folder Name Title
    std::string folder_display = "FOLDER: " + folder_path;
    if (folder_display.length() > 38) {
        folder_display = "FOLDER: ..." + folder_path.substr(folder_path.length() - 32);
    }
    std::string count_str = " (" + std::to_string(items.size()) + " Videos)";
    draw_ui_text(folder_display + count_str, 24.0f, header_y + 18.0f, 15.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);

    // "SELECT FOLDER" Button on Top Right
    float chg_btn_w = 170.0f;
    float chg_btn_h = 32.0f;
    float chg_btn_x = win_w - chg_btn_w - 24.0f;
    float chg_btn_y = header_y + 12.0f;

    bool chg_hover = (mouse_x >= chg_btn_x && mouse_x <= chg_btn_x + chg_btn_w &&
                      mouse_y >= chg_btn_y && mouse_y <= chg_btn_y + chg_btn_h);

    std::string sel_txt = "SELECT FOLDER";
    float sel_w = get_text_width(sel_txt, 14.0f);
    float sel_x = chg_btn_x + (chg_btn_w - sel_w) / 2.0f;

    if (chg_hover) {
        draw_ui_rounded_rect(chg_btn_x, chg_btn_y, chg_btn_w, chg_btn_h, 16.0f, 0.0f, 0.85f, 1.0f, 0.90f, 1.0f, win_w, win_h);
        draw_ui_text(sel_txt, sel_x, chg_btn_y + 8.0f, 14.0f, 0.02f, 0.05f, 0.10f, 1.0f, 1.0f, win_w, win_h);
    } else {
        draw_ui_rounded_rect(chg_btn_x, chg_btn_y, chg_btn_w, chg_btn_h, 16.0f, 0.0f, 0.40f, 0.70f, 0.45f, 1.0f, win_w, win_h);
        draw_ui_text(sel_txt, sel_x, chg_btn_y + 8.0f, 14.0f, 1.0f, 1.0f, 1.0f, 0.95f, 1.0f, win_w, win_h);
    }

    if (items.empty()) {
        std::string empty_msg = "NO MEDIA FILES FOUND IN THIS FOLDER";
        float msg_w = get_text_width(empty_msg, 16.0f);
        draw_ui_text(empty_msg, (win_w - msg_w) / 2.0f, win_h / 2.0f - 20.0f, 16.0f, 0.8f, 0.8f, 0.8f, 0.7f, 1.0f, win_w, win_h);
        return;
    }

    // 2. Responsive Grid Geometry Calculation
    float grid_top = header_y + header_h + 16.0f;
    float pad_x = 24.0f;
    float gap_x = 18.0f;
    float gap_y = 20.0f;
    float avail_w = static_cast<float>(win_w) - 2.0f * pad_x;

    int num_cols = std::max(2, std::min(6, static_cast<int>((avail_w + gap_x) / 240.0f)));
    float card_w = (avail_w - (num_cols - 1) * gap_x) / num_cols;
    float thumb_h = card_w * (9.0f / 16.0f); // 16:9 thumbnail aspect ratio
    float text_h = 44.0f;
    float card_h = thumb_h + text_h;

    int num_rows = (static_cast<int>(items.size()) + num_cols - 1) / num_cols;
    int scroll_row = std::max(0, std::min(num_rows - 1, scroll_offset));

    for (size_t i = 0; i < items.size(); i++) {
        int r = static_cast<int>(i) / num_cols;
        int c = static_cast<int>(i) % num_cols;

        int row_on_screen = r - scroll_row;
        if (row_on_screen < 0) continue;

        float card_x = pad_x + c * (card_w + gap_x);
        float card_y = grid_top + row_on_screen * (card_h + gap_y);

        if (card_y + card_h > win_h + 50.0f) break;

        bool card_hover = (mouse_x >= card_x && mouse_x <= card_x + card_w &&
                           mouse_y >= card_y && mouse_y <= card_y + card_h);

        // Card Outer Border and Background Panel
        if (card_hover) {
            draw_ui_rounded_rect(card_x - 3.0f, card_y - 3.0f, card_w + 6.0f, card_h + 6.0f, 14.0f, 0.0f, 0.85f, 1.0f, 0.40f, 1.0f, win_w, win_h);
            draw_ui_rounded_rect(card_x, card_y, card_w, card_h, 12.0f, 0.08f, 0.12f, 0.20f, 0.98f, 1.0f, win_w, win_h);
        } else {
            draw_ui_rounded_rect(card_x, card_y, card_w, card_h, 12.0f, 0.05f, 0.07f, 0.12f, 0.90f, 1.0f, win_w, win_h);
        }

        // Thumbnail Preview
        if (items[i].thumb_tex != 0) {
            draw_ui_icon(items[i].thumb_tex, card_x, card_y, card_w, thumb_h, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);
        } else {
            // Placeholder dark container with glowing play icon
            draw_ui_rounded_rect(card_x, card_y, card_w, thumb_h, 10.0f, 0.03f, 0.04f, 0.08f, 1.0f, 1.0f, win_w, win_h);
            float ph_icon_sz = 32.0f;
            draw_ui_icon(tex_icon_play, card_x + (card_w - ph_icon_sz) / 2.0f, card_y + (thumb_h - ph_icon_sz) / 2.0f,
                         ph_icon_sz, ph_icon_sz, 0.0f, 0.85f, 1.0f, 0.75f, 1.0f, win_w, win_h);
        }

        // Thumbnail Top-Left Index Tag
        char idx_buf[16];
        snprintf(idx_buf, sizeof(idx_buf), "#%02d", static_cast<int>(i) + 1);
        float tag_w = 34.0f, tag_h = 18.0f;
        draw_ui_rounded_rect(card_x + 6.0f, card_y + 6.0f, tag_w, tag_h, 4.0f, 0.0f, 0.0f, 0.0f, 0.75f, 1.0f, win_w, win_h);
        draw_ui_text(idx_buf, card_x + 9.0f, card_y + 3.0f, 11.0f, 0.0f, 0.90f, 1.0f, 0.95f, 1.0f, win_w, win_h);

        // Thumbnail Bottom-Right DURATION BADGE (توقيت / مدة الفيديو)
        std::string dur_str = items[i].duration_str.empty() ? "00:00" : items[i].duration_str;
        float dur_w = get_text_width(dur_str, 11.0f) + 12.0f;
        float dur_h = 18.0f;
        float dur_x = card_x + card_w - dur_w - 6.0f;
        float dur_y = card_y + thumb_h - dur_h - 6.0f;
        draw_ui_rounded_rect(dur_x, dur_y, dur_w, dur_h, 4.0f, 0.0f, 0.0f, 0.0f, 0.85f, 1.0f, win_w, win_h);
        draw_ui_text(dur_str, dur_x + 6.0f, dur_y + 3.0f, 11.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);

        // Hover Play Overlay in Center of Thumbnail
        if (card_hover) {
            float play_ovl_sz = 38.0f;
            float ovl_x = card_x + (card_w - play_ovl_sz) / 2.0f;
            float ovl_y = card_y + (thumb_h - play_ovl_sz) / 2.0f;
            draw_ui_circle(ovl_x + play_ovl_sz / 2.0f, ovl_y + play_ovl_sz / 2.0f, 22.0f, 0.0f, 0.85f, 1.0f, 0.90f, 1.0f, win_w, win_h);
            draw_ui_icon(tex_icon_play, ovl_x + 2.0f, ovl_y + 2.0f, play_ovl_sz - 4.0f, play_ovl_sz - 4.0f, 0.02f, 0.05f, 0.10f, 1.0f, 1.0f, win_w, win_h);
        }

        // Title and details in bottom part of card
        std::string fname = items[i].filename;
        float avail_title_w = card_w - 16.0f;
        if (get_text_width(fname, 13.5f) > avail_title_w && fname.length() > 6) {
            while (fname.length() > 4 && get_text_width(fname + "...", 13.5f) > avail_title_w) {
                fname.pop_back();
            }
            fname += "...";
        }
        float title_y = card_y + thumb_h + 8.0f;
        if (card_hover) {
            draw_ui_text(fname, card_x + 8.0f, title_y, 13.5f, 0.0f, 0.90f, 1.0f, 1.0f, 1.0f, win_w, win_h);
        } else {
            draw_ui_text(fname, card_x + 8.0f, title_y, 13.5f, 1.0f, 1.0f, 1.0f, 0.95f, 1.0f, win_w, win_h);
        }

        // Subtitle text (Size & Format)
        std::string sub_info = items[i].size_str;
        draw_ui_text(sub_info, card_x + 8.0f, title_y + 20.0f, 11.0f, 0.60f, 0.70f, 0.80f, 0.75f, 1.0f, win_w, win_h);
    }
}

void ShaderRenderer::init_vlc_menus() {
    if (!vlc_menus.empty()) return;

    VlcMenuCategory media;
    media.title = "Media";
    media.items = {
        {"Open File...", "Ctrl+O", VlcMenuAction::MEDIA_OPEN_FILE},
        {"Open Folder...", "O", VlcMenuAction::MEDIA_OPEN_FOLDER},
        {"Quit Application", "Esc", VlcMenuAction::MEDIA_QUIT}
    };

    VlcMenuCategory playback;
    playback.title = "Playback";
    playback.items = {
        {"Play / Pause", "Space", VlcMenuAction::PLAYBACK_TOGGLE_PAUSE},
        {"Forward 5s", "Right", VlcMenuAction::PLAYBACK_FORWARD},
        {"Rewind 5s", "Left", VlcMenuAction::PLAYBACK_REWIND}
    };

    VlcMenuCategory audio;
    audio.title = "Audio";
    audio.items = {
        {"Volume Up", "Up", VlcMenuAction::AUDIO_VOL_UP},
        {"Volume Down", "Down", VlcMenuAction::AUDIO_VOL_DOWN},
        {"Mute Audio", "M", VlcMenuAction::AUDIO_MUTE}
    };

    VlcMenuCategory video;
    video.title = "Video";
    video.items = {
        {"Fullscreen", "F", VlcMenuAction::VIDEO_FULLSCREEN},
        {"Aspect Ratio", "A", VlcMenuAction::VIDEO_ASPECT_RATIO}
    };

    VlcMenuCategory subtitle;
    subtitle.title = "Subtitle";
    subtitle.items = {
        {"Toggle Subtitles", "S", VlcMenuAction::SUBTITLE_TOGGLE},
        {"Delay Subtitle (-0.1s)", "Z", VlcMenuAction::SUBTITLE_DELAY_MINUS},
        {"Advance Subtitle (+0.1s)", "X", VlcMenuAction::SUBTITLE_DELAY_PLUS}
    };

    VlcMenuCategory tools;
    tools.title = "Tools";
    tools.items = {
        {"Video Telemetry & Stats", "I", VlcMenuAction::TOOLS_STATS}
    };

    VlcMenuCategory view;
    view.title = "View";
    view.items = {
        {"Folder Gallery", "G", VlcMenuAction::VIEW_GALLERY}
    };

    VlcMenuCategory help;
    help.title = "Help";
    help.items = {
        {"About VMP (Video Max Player)", "", VlcMenuAction::HELP_ABOUT}
    };

    vlc_menus = { media, playback, audio, video, subtitle, tools, view, help };
}

void ShaderRenderer::render_vlc_menu_bar(int win_w, int win_h, double mouse_x, double mouse_y,
                                         int active_menu_idx, float menu_alpha) {
    (void)mouse_x;
    (void)mouse_y;
    if (win_w <= 0 || win_h <= 0 || menu_alpha <= 0.01f) return;
    init_vlc_menus();

    float menu_h = 26.0f;
    float font_sz = 12.5f;

    // VLC Menu Bar dark background (#21232a)
    draw_ui_rect(0, 0, static_cast<float>(win_w), menu_h, 0.13f, 0.14f, 0.17f, 1.0f, menu_alpha, win_w, win_h);
    // Subtle 1px bottom border line (#18191f)
    draw_ui_rect(0, menu_h - 1.0f, static_cast<float>(win_w), 1.0f, 0.09f, 0.10f, 0.12f, 1.0f, menu_alpha, win_w, win_h);

    float cur_x = 8.0f;
    for (size_t i = 0; i < vlc_menus.size(); ++i) {
        auto& cat = vlc_menus[i];
        float text_w = get_text_width(cat.title, font_sz);
        cat.x = cur_x;
        cat.width = text_w + 14.0f;

        bool is_active = (static_cast<int>(i) == active_menu_idx);

        if (is_active) {
            // VLC active menu item highlight (when clicked / open)
            draw_ui_rounded_rect(cat.x, 2.0f, cat.width, menu_h - 4.0f, 3.0f, 
                                 0.0f, 0.45f, 0.85f, 0.50f, menu_alpha, win_w, win_h);
            draw_ui_text(cat.title, cat.x + 7.0f, 4.5f, font_sz, 
                         1.0f, 1.0f, 1.0f, 1.0f, menu_alpha, win_w, win_h);
        } else {
            // Normal state: No halo or box on hover, purely clean like VLC
            draw_ui_text(cat.title, cat.x + 7.0f, 4.5f, font_sz, 
                         0.90f, 0.92f, 0.94f, 0.95f, menu_alpha, win_w, win_h);
        }

        cur_x += cat.width + 2.0f;
    }
}

void ShaderRenderer::render_vlc_dropdown(int win_w, int win_h, double mouse_x, double mouse_y, 
                                         int menu_idx, float custom_x, float custom_y) {
    if (win_w <= 0 || win_h <= 0 || menu_idx < 0 || menu_idx >= static_cast<int>(vlc_menus.size())) return;

    const auto& cat = vlc_menus[menu_idx];
    if (cat.items.empty()) return;

    float menu_h = 26.0f;
    float card_w = 260.0f;
    float item_h = 26.0f;
    float card_h = static_cast<float>(cat.items.size()) * item_h + 8.0f;

    float drop_x = (custom_x >= 0.0f) ? custom_x : std::max(4.0f, std::min(cat.x, static_cast<float>(win_w) - card_w - 10.0f));
    float drop_y = (custom_y >= 0.0f) ? custom_y : menu_h;
    if (drop_y + card_h > static_cast<float>(win_h) - 10.0f) {
        drop_y = std::max(0.0f, static_cast<float>(win_h) - card_h - 10.0f);
    }

    // Dropdown Outer Border (VLC dark gray border)
    draw_ui_rounded_rect(drop_x - 1.0f, drop_y - 1.0f, card_w + 2.0f, card_h + 2.0f, 4.0f, 
                         0.23f, 0.25f, 0.30f, 0.95f, 1.0f, win_w, win_h);

    // Dropdown Background Panel (VLC dark card #20232b)
    draw_ui_rounded_rect(drop_x, drop_y, card_w, card_h, 3.0f, 
                         0.125f, 0.135f, 0.17f, 0.98f, 1.0f, win_w, win_h);

    for (size_t i = 0; i < cat.items.size(); ++i) {
        const auto& item = cat.items[i];
        float it_y = drop_y + 4.0f + static_cast<float>(i) * item_h;

        bool it_hover = (mouse_x >= drop_x + 2.0f && mouse_x <= drop_x + card_w - 2.0f &&
                         mouse_y >= it_y && mouse_y < it_y + item_h);

        if (it_hover) {
            draw_ui_rounded_rect(drop_x + 2.0f, it_y, card_w - 4.0f, item_h, 3.0f, 
                                 0.0f, 0.42f, 0.80f, 0.75f, 1.0f, win_w, win_h);
            draw_ui_text(item.label, drop_x + 12.0f, it_y + 5.0f, 12.0f, 
                         1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);
        } else {
            draw_ui_text(item.label, drop_x + 12.0f, it_y + 5.0f, 12.0f, 
                         0.90f, 0.92f, 0.95f, 0.95f, 1.0f, win_w, win_h);
        }

        if (!item.shortcut.empty()) {
            float sc_w = get_text_width(item.shortcut, 11.0f);
            float sc_x = drop_x + card_w - sc_w - 12.0f;
            draw_ui_text(item.shortcut, sc_x, it_y + 6.0f, 11.0f, 
                         0.55f, 0.62f, 0.72f, 0.85f, 1.0f, win_w, win_h);
        }
    }
}

int ShaderRenderer::hit_test_vlc_menu_bar(int win_w, int win_h, double mouse_x, double mouse_y) {
    if (win_w <= 0 || win_h <= 0) return -1;
    init_vlc_menus();
    float menu_h = 26.0f;
    if (mouse_y < 0.0 || mouse_y > menu_h) return -1;

    for (size_t i = 0; i < vlc_menus.size(); ++i) {
        if (mouse_x >= vlc_menus[i].x && mouse_x < vlc_menus[i].x + vlc_menus[i].width) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

VlcMenuAction ShaderRenderer::hit_test_vlc_dropdown(int win_w, int win_h, double mouse_x, double mouse_y, 
                                                    int menu_idx, float custom_x, float custom_y) {
    if (win_w <= 0 || win_h <= 0 || menu_idx < 0 || menu_idx >= static_cast<int>(vlc_menus.size())) {
        return VlcMenuAction::NONE;
    }

    const auto& cat = vlc_menus[menu_idx];
    if (cat.items.empty()) return VlcMenuAction::NONE;

    float menu_h = 26.0f;
    float card_w = 260.0f;
    float item_h = 26.0f;
    float card_h = static_cast<float>(cat.items.size()) * item_h + 8.0f;

    float drop_x = (custom_x >= 0.0f) ? custom_x : std::max(4.0f, std::min(cat.x, static_cast<float>(win_w) - card_w - 10.0f));
    float drop_y = (custom_y >= 0.0f) ? custom_y : menu_h;
    if (drop_y + card_h > static_cast<float>(win_h) - 10.0f) {
        drop_y = std::max(0.0f, static_cast<float>(win_h) - card_h - 10.0f);
    }

    if (mouse_x < drop_x || mouse_x > drop_x + card_w ||
        mouse_y < drop_y || mouse_y > drop_y + card_h) {
        return VlcMenuAction::NONE;
    }

    for (size_t i = 0; i < cat.items.size(); ++i) {
        float it_y = drop_y + 4.0f + static_cast<float>(i) * item_h;
        if (mouse_y >= it_y && mouse_y < it_y + item_h) {
            return cat.items[i].action;
        }
    }

    return VlcMenuAction::NONE;
}

bool ShaderRenderer::is_mouse_inside_dropdown(int win_w, int win_h, double mouse_x, double mouse_y, 
                                              int menu_idx, float custom_x, float custom_y) {
    if (win_w <= 0 || win_h <= 0 || menu_idx < 0 || menu_idx >= static_cast<int>(vlc_menus.size())) return false;
    const auto& cat = vlc_menus[menu_idx];
    float menu_h = 26.0f;
    float card_w = 260.0f;
    float item_h = 26.0f;
    float card_h = static_cast<float>(cat.items.size()) * item_h + 8.0f;
    float drop_x = (custom_x >= 0.0f) ? custom_x : std::max(4.0f, std::min(cat.x, static_cast<float>(win_w) - card_w - 10.0f));
    float drop_y = (custom_y >= 0.0f) ? custom_y : menu_h;
    if (drop_y + card_h > static_cast<float>(win_h) - 10.0f) {
        drop_y = std::max(0.0f, static_cast<float>(win_h) - card_h - 10.0f);
    }
    return (mouse_x >= drop_x && mouse_x <= drop_x + card_w &&
            mouse_y >= drop_y && mouse_y <= drop_y + card_h);
}

void ShaderRenderer::render_vlc_context_menu(int win_w, int win_h, double mouse_x, double mouse_y,
                                             float ctx_x, float ctx_y, int active_submenu_idx) {
    if (win_w <= 0 || win_h <= 0) return;
    init_vlc_menus();

    float card_w = 170.0f;
    float item_h = 26.0f;
    float card_h = static_cast<float>(vlc_menus.size()) * item_h + 8.0f;

    float root_x = std::max(4.0f, std::min(ctx_x, static_cast<float>(win_w) - card_w - 10.0f));
    float root_y = std::max(4.0f, std::min(ctx_y, static_cast<float>(win_h) - card_h - 10.0f));

    // Outer border
    draw_ui_rounded_rect(root_x - 1.0f, root_y - 1.0f, card_w + 2.0f, card_h + 2.0f, 4.0f,
                         0.23f, 0.25f, 0.30f, 0.95f, 1.0f, win_w, win_h);
    // Background
    draw_ui_rounded_rect(root_x, root_y, card_w, card_h, 3.0f,
                         0.125f, 0.135f, 0.17f, 0.98f, 1.0f, win_w, win_h);

    for (size_t i = 0; i < vlc_menus.size(); ++i) {
        float it_y = root_y + 4.0f + static_cast<float>(i) * item_h;
        bool it_hover = (mouse_x >= root_x + 2.0f && mouse_x <= root_x + card_w - 2.0f &&
                         mouse_y >= it_y && mouse_y < it_y + item_h);
        bool is_sub_active = (static_cast<int>(i) == active_submenu_idx);

        if (it_hover || is_sub_active) {
            draw_ui_rounded_rect(root_x + 2.0f, it_y, card_w - 4.0f, item_h, 3.0f,
                                 0.0f, 0.42f, 0.80f, 0.75f, 1.0f, win_w, win_h);
            draw_ui_text(vlc_menus[i].title, root_x + 12.0f, it_y + 5.0f, 12.0f,
                         1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);
            draw_ui_text(">", root_x + card_w - 16.0f, it_y + 5.0f, 12.0f,
                         1.0f, 1.0f, 1.0f, 1.0f, 1.0f, win_w, win_h);
        } else {
            draw_ui_text(vlc_menus[i].title, root_x + 12.0f, it_y + 5.0f, 12.0f,
                         0.90f, 0.92f, 0.95f, 0.95f, 1.0f, win_w, win_h);
            draw_ui_text(">", root_x + card_w - 16.0f, it_y + 5.0f, 12.0f,
                         0.55f, 0.62f, 0.72f, 0.85f, 1.0f, win_w, win_h);
        }
    }

    // Render active submenu if open
    if (active_submenu_idx >= 0 && active_submenu_idx < static_cast<int>(vlc_menus.size())) {
        float sub_x = root_x + card_w + 2.0f;
        if (sub_x + 260.0f > static_cast<float>(win_w)) {
            sub_x = root_x - 262.0f;
        }
        float sub_y = root_y + 4.0f + static_cast<float>(active_submenu_idx) * item_h;
        render_vlc_dropdown(win_w, win_h, mouse_x, mouse_y, active_submenu_idx, sub_x, sub_y);
    }
}

int ShaderRenderer::hit_test_vlc_context_menu_category(int win_w, int win_h, double mouse_x, double mouse_y,
                                                       float ctx_x, float ctx_y) {
    if (win_w <= 0 || win_h <= 0) return -1;
    init_vlc_menus();

    float card_w = 170.0f;
    float item_h = 26.0f;
    float card_h = static_cast<float>(vlc_menus.size()) * item_h + 8.0f;

    float root_x = std::max(4.0f, std::min(ctx_x, static_cast<float>(win_w) - card_w - 10.0f));
    float root_y = std::max(4.0f, std::min(ctx_y, static_cast<float>(win_h) - card_h - 10.0f));

    if (mouse_x < root_x || mouse_x > root_x + card_w ||
        mouse_y < root_y || mouse_y > root_y + card_h) {
        return -1;
    }

    for (size_t i = 0; i < vlc_menus.size(); ++i) {
        float it_y = root_y + 4.0f + static_cast<float>(i) * item_h;
        if (mouse_y >= it_y && mouse_y < it_y + item_h) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

bool ShaderRenderer::is_mouse_inside_context_menu(int win_w, int win_h, double mouse_x, double mouse_y,
                                                  float ctx_x, float ctx_y, int active_submenu_idx) {
    if (win_w <= 0 || win_h <= 0) return false;
    init_vlc_menus();

    float card_w = 170.0f;
    float item_h = 26.0f;
    float card_h = static_cast<float>(vlc_menus.size()) * item_h + 8.0f;

    float root_x = std::max(4.0f, std::min(ctx_x, static_cast<float>(win_w) - card_w - 10.0f));
    float root_y = std::max(4.0f, std::min(ctx_y, static_cast<float>(win_h) - card_h - 10.0f));

    if (mouse_x >= root_x && mouse_x <= root_x + card_w &&
        mouse_y >= root_y && mouse_y <= root_y + card_h) {
        return true;
    }

    if (active_submenu_idx >= 0 && active_submenu_idx < static_cast<int>(vlc_menus.size())) {
        float sub_x = root_x + card_w + 2.0f;
        if (sub_x + 260.0f > static_cast<float>(win_w)) {
            sub_x = root_x - 262.0f;
        }
        float sub_y = root_y + 4.0f + static_cast<float>(active_submenu_idx) * item_h;
        return is_mouse_inside_dropdown(win_w, win_h, mouse_x, mouse_y, active_submenu_idx, sub_x, sub_y);
    }

    return false;
}
