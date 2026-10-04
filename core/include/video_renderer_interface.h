#ifndef VMP_VIDEO_RENDERER_INTERFACE_H
#define VMP_VIDEO_RENDERER_INTERFACE_H

#include <cstdint>

class IVideoRenderer {
public:
    virtual ~IVideoRenderer() = default;
    virtual void upload_yuv_frame(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                  int y_linesize, int u_linesize, int v_linesize,
                                  int width, int height) = 0;
    virtual void upload_nv12_frame(uint8_t* y_plane, uint8_t* uv_plane,
                                   int y_linesize, int uv_linesize,
                                   int width, int height) = 0;
    virtual void upload_p010_frame(uint8_t* y_plane, uint8_t* uv_plane,
                                   int y_linesize, int uv_linesize,
                                   int width, int height) = 0;
    virtual void upload_yuv10_frame(uint8_t* y_plane, uint8_t* u_plane, uint8_t* v_plane,
                                    int y_linesize, int u_linesize, int v_linesize,
                                    int width, int height) = 0;
    virtual void upload_rgb_frame(uint8_t* rgb_data, int width, int height) = 0;
    virtual void render(int window_width, int window_height, bool menu_bar_visible = true) = 0;
};

#endif // VMP_VIDEO_RENDERER_INTERFACE_H
