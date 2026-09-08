# VMP (Video Max Player) v0.0.2-beta - Desktop Application

Ultra-native, high-performance Linux desktop video player with hardware decoding (VA-API/NVDEC), OpenGL HDR tone mapping, custom shaders, real-time telemetry, and VLC-style menu controls.

> ⚠️ **Important:** VMP is dedicated **strictly to video playback**. Opening image files is intentionally rejected to protect system image viewing associations.

---

## 🚀 Features

- **Video-Only Architecture:** Dedicated media pipeline engineered for high-bitrate 4K/8K video playback.
- **Hardware Acceleration:** Auto-detection of VA-API (Intel/AMD) and NVDEC/CUDA (NVIDIA).
- **Hot-Toggle HW/CPU Engine:** Instantly switch between GPU hardware acceleration and multi-threaded AVX2 CPU decoder using `Shift+H`.
- **Custom Post-Processing Shaders:** Contrast-Adaptive Sharpening (CAS), Cinematic Warmth, Cool Night, Retro CRT, and HDR Tone Mapping (ACES Filmic).
- **Precision Audio Sync:** Microsecond-level clock synchronizer driven by SDL2.
- **Subtitles & Resumption:** Embedded subtitle selector, external `.srt`/`.vtt` drag & drop, and automatic playback position saving.
- **Interactive UI & Menus:** VLC-style top menu bar (`Media`, `Audio`, `Video`, `Subtitle`, `Tools`, `View`, `Help`), right-click context menu, timeline hover thumbnails, volume wheel, and hotkey controls.

---

## 📦 Requirements & Dependencies

### Ubuntu / Debian / Kali / Linux Mint:
```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
                    libavcodec-dev libavformat-dev libswscale-dev \
                    libswresample-dev libavutil-dev \
                    libgl1-mesa-dev libglfw3-dev libsdl2-dev \
                    libfreetype-dev libfribidi-dev libva-dev
```

### Fedora / RHEL / CentOS:
```bash
sudo dnf install -y gcc-c++ cmake pkgconf \
                    ffmpeg-free-devel SDL2-devel glfw-devel \
                    mesa-libGL-devel freetype-devel fribidi-devel libva-devel
```

### Arch Linux / Manjaro:
```bash
sudo pacman -S --needed base-devel cmake pkgconf \
                        ffmpeg sdl2 glfw-x11 mesa \
                        freetype2 fribidi libva
```

---

## 🛠️ Build Instructions (Standalone)

```bash
cd desktop
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binaries generated:
- `build/vmp_engine` (Desktop executable)
- `build/vmp_desktop` (Symlink alias)
- `build/vmp` (Short symlink alias)
- `build/libvmp.so` (Shared library)

---

## 📥 Desktop Installation on Linux

### Method 1: Automated 1-Click Installer (Recommended)

```bash
./install.sh
```

**Actions performed automatically by `install.sh`:**
- Builds `vmp_engine` and `libvmp.so` in release mode.
- Installs the desktop launcher entry to `~/.local/share/applications/vmp.desktop`.
- Places a launcher shortcut directly onto your desktop (`~/Desktop/VMP.desktop`).
- Installs multi-resolution icons (16px to 512px and scalable SVG) into `~/.local/share/icons/hicolor/`.
- Associates video MIME types (`.mp4`, `.mkv`, `.webm`, `.avi`, `.mov`, etc.) with VMP.
- Protects default image viewers (e.g. eog, ristretto) from being overridden.
- Installs `vmp_cli` into `~/.local/bin/`.

---

### Method 2: Manual Installation

```bash
# 1. Install binary
mkdir -p ~/.local/bin
cp build/vmp_engine ~/.local/bin/vmp_engine
chmod +x ~/.local/bin/vmp_engine

# 2. Install desktop shortcut
mkdir -p ~/.local/share/applications
sed "s|Exec=vmp_engine|Exec=$HOME/.local/bin/vmp_engine|g" vmp.desktop > ~/.local/share/applications/vmp.desktop
chmod +x ~/.local/share/applications/vmp.desktop

# 3. Install icon
mkdir -p ~/.local/share/icons/hicolor/scalable/apps
cp assets/icons/vmp_app_icon.svg ~/.local/share/icons/hicolor/scalable/apps/vmp.svg

# 4. Update desktop database
update-desktop-database ~/.local/share/applications 2>/dev/null || true
gtk-update-icon-cache -f -t ~/.local/share/icons/hicolor 2>/dev/null || true
```

---

## 💻 Running the Desktop Player

```bash
# Launch empty player (ready for Drag & Drop)
./build/vmp_engine

# Open a video directly
./build/vmp_engine /path/to/video.mp4

# Launch and export playback session telemetry
./build/vmp_engine /path/to/video.mp4 --export-stats telemetry.json
```

---

## ⌨️ Keyboard & Mouse Shortcuts

| Key / Action | Function |
| :--- | :--- |
| **Space** / Left Click | Play / Pause video |
| **Left / Right** | Seek backward / forward 5 seconds |
| **Up / Down** | Volume up / down (+5% / -5%) |
| **M** | Mute / Unmute audio |
| **F** / **F11** | Toggle Fullscreen |
| **Shift + H** | **Toggle Hardware Acceleration (VA-API/NVDEC) vs Multi-Threaded AVX2 CPU** |
| **S** | Cycle Shader Filters (HDR, Sharpen, Warm, Cool, CRT, Off) |
| **T** | Toggle Telemetry Overlay (FPS, Bitrate, Jitter, HW Decoder) |
| **C** | Cycle Subtitle Tracks |
| **L** | Toggle Loop Playback |
| **Right Click** | Open VLC-Style Context Menu |
| **Top Bar** | Click Menus (Media, Audio, Video, Subtitles, Tools, View, Help) |
| **Mouse Wheel** | Adjust Volume |
| **Drag & Drop** | Drop video files or `.srt` subtitle files into window |
| **Q** / **Esc** | Quit player |

---

## 🗑️ Uninstallation

```bash
rm -f ~/.local/share/applications/vmp.desktop
rm -f ~/Desktop/VMP.desktop
rm -f ~/.local/bin/vmp_engine
rm -f ~/.local/share/icons/hicolor/scalable/apps/vmp.svg
rm -f ~/.local/share/icons/hicolor/*/apps/vmp.png
```
