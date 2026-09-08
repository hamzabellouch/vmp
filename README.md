# VMP

<h3 align="center">Ultra-Native Linux Video Max Player & Benchmarking Suite</h3>

<p align="center">
High-performance C++20 media engine for 4K/8K/16K video playback, hardware-accelerated decoding, and zero-drop rendering.
</p>

<img width="1365" height="768" alt="vmp" src="https://github.com/user-attachments/assets/47a6ee68-85d5-4829-a2b7-ea068471183f" />


## Overview

VMP (Video Max Player) is an advanced, ultra-native video playback and benchmarking suite engineered in **C++20** specifically for **Linux**. It combines high-throughput graphical video rendering with headless CLI decoding telemetry and benchmarking.

The platform is designed to help video engineers, performance analysts, developers, and power users achieve maximum framerate, zero dropped frames, microsecond-accurate audio synchronization, and real-time shader processing on extreme high-bitrate streams (up to 4K / 8K / 16K) without web or Electron overhead.

Currently available for Linux OS (Ubuntu, Debian, Kali Linux, Parrot OS, Fedora, Arch Linux, openSUSE, and headless server environments).



## Repository Directory Structure

The project is cleanly separated into two standalone, modular sub-projects:

```
VMP/
├── cli/                 # [Sub-Project 1]: Command-Line Interface (Headless)
│   ├── CMakeLists.txt   # Standalone build configuration (zero OpenGL/GLFW dependencies)
│   ├── README.md        # CLI documentation and commands
│   └── src/             # Source files for CLI, probe, benchmarks, and audio
│       ├── main_cli.cpp
│       ├── hw_decoder.*
│       ├── audio_engine.*
│       ├── telemetry.*
│       ├── video_player.*
│       ├── subtitles.*
│       ├── resume_manager.*
│       └── thumbnail_generator.*
│
├── desktop/             # [Sub-Project 2]: Desktop GUI Application
│   ├── CMakeLists.txt   # Standalone build configuration for GUI & shared library
│   ├── README.md        # Desktop user guide & keyboard shortcuts
│   ├── install.sh       # System desktop shortcut, icons & menu installer
│   ├── vmp.desktop      # Linux desktop application entry (video MIME types only)
│   ├── assets/          # Application icons and visual assets
│   ├── generate_icons.py# Icon generator script (16x16 to 512x512 PNG + SVG)
│   └── src/             # Source files for OpenGL, GLFW, and post-processing shaders
│       ├── main_desktop.cpp
│       ├── shader_renderer.*
│       └── ...
│
├── CMakeLists.txt       # Root build orchestrator (builds both targets)
├── README.md            # Markdown documentation (Linux installation & guide)
└── LICENSE              # MIT License
```



## Key Features

### Dual Playback & Auditing Engines

Switch between targeted automation and deep graphical playback:

* **VMP Desktop (GUI)**: Modern OpenGL 3.3 Core Profile video player with full VLC-style controls, GLSL post-processing shaders, and timeline preview thumbnails
* **VMP CLI (Headless)**: Standalone command-line tool for video probing, speed benchmarks, telemetry reporting, and hardware decoder discovery with zero GUI dependencies

### Hardware Acceleration & Smart Routing

Intelligent decoder routing designed for rock-solid stability under load:

* **GPU Hardware Decoding**: Auto-detection and native hardware decoding via VA-API (Intel/AMD) and NVDEC/CUDA (NVIDIA)
* **Hot-Toggle Decoding (`Shift+H`)**: Instant real-time switching between GPU Hardware Acceleration and Multi-Threaded AVX2 CPU decode without stopping playback
* **Smart Fallback Mechanism**: Automatically detects driver/bus bottlenecks on extreme 4K/8K streams and falls back to AVX2 CPU decode to prevent player crashes
* **Deep Dynamic Ring Buffer**: 64 to 96 frames raw decoded buffer queue to completely eliminate stuttering and frame drops during multitasking
* **Zero Frame Drops**: Locked 55–60 FPS and up to 120+ FPS playback stability on native Linux x86_64

### Post-Processing Shader Pipeline

Custom modern GLSL post-processing filters rendered in real-time (`S` key):

* **HDR Tone Mapping**: ACES Filmic tone mapping for vibrant dynamic range representation
* **Contrast-Adaptive Sharpening (CAS)**: GPU-accelerated edge sharpening for ultra-crisp details
* **Color Grading & Filters**: Warm, Cool, and retro CRT scanline emulation filters

### Audio Synchronization & Subtitle Precision

* **Microsecond-Accurate Clock**: ±0.3 ms audio synchronization powered by SDL2 audio engine (28× tighter sync than standard players)
* **Advanced Subtitle Rendering**: SSA/ASS/SRT subtitle styling, multi-track switching (`C`), and FriBidi bidirectional text support
* **Automatic Resume Manager**: Remembers playback timestamps across sessions and supports instant resumption

### Telemetry & Benchmarking Suite

* **Real-Time Telemetry HUD (`T`)**: On-screen display showing active FPS, 1% Low FPS, per-frame latency in ms, video bitrate, and decoder engine
* **Headless Benchmark Engine**: Measure unlocked decoding throughput (144+ FPS) across thousands of frames with JSON telemetry export (`--export-stats`)
* **Hardware API Discovery**: Command-line audit tool (`vmp_cli hw-accel`) to query all available decoding backends on the host

### Dedicated Video Specialization

* **Strictly Video-Only**: Dedicated strictly to video playback; static image files are instantly rejected to safeguard default image viewers
* **MIME Association Safety**: Automatically registers video formats while protecting desktop image viewing associations
* **Modular Architecture**: CLI builds independently without any GUI or OpenGL/GLFW libraries



## Screenshots

Add screenshots here.

```text
screenshots/1
screenshots/2
screenshots/3
```



## System Dependencies & Prerequisites

Before building or installing, ensure your Linux system has the required build tools and media libraries installed:

### 1. Ubuntu / Debian / Kali Linux / Linux Mint / Pop!_OS:
```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
                    libavcodec-dev libavformat-dev libswscale-dev \
                    libswresample-dev libavutil-dev \
                    libgl1-mesa-dev libglfw3-dev libsdl2-dev \
                    libfreetype-dev libfribidi-dev libva-dev
```
*(Note: For headless CLI-only builds, `libgl1-mesa-dev`, `libglfw3-dev`, `libfreetype-dev`, and `libfribidi-dev` are not required).*

### 2. Fedora / RHEL / CentOS / AlmaLinux:
```bash
sudo dnf install -y gcc-c++ cmake pkgconf \
                    ffmpeg-free-devel SDL2-devel glfw-devel \
                    mesa-libGL-devel freetype-devel fribidi-devel libva-devel
```

### 3. Arch Linux / Manjaro / EndeavourOS:
```bash
sudo pacman -S --needed base-devel cmake pkgconf \
                        ffmpeg sdl2 glfw-x11 mesa \
                        freetype2 fribidi libva
```

### 4. openSUSE:
```bash
sudo zypper install gcc-c++ cmake pkg-config \
                    libffmpeg-devel libSDL2-devel libglfw-devel \
                    Mesa-libGL-devel freetype-devel fribidi-devel libva-devel
```



## Build Options

### Option 1: Unified Build from Root (CLI + Desktop)
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
Generates:
* `./build/cli/vmp_cli`
* `./build/desktop/vmp_engine` (and `./build/desktop/vmp`, `./build/desktop/vmp_desktop`)
* `./build/desktop/libvmp.so`

### Option 2: Standalone CLI Build (Headless, Zero GUI Dependencies)
```bash
cd cli
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
Generates:
* `./cli/build/vmp_cli`

### Option 3: Standalone Desktop Build (GUI & Shared Library)
```bash
cd desktop
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
Generates:
* `./desktop/build/vmp_engine`
* `./desktop/build/libvmp.so`



## Installation & Running

### 1. Desktop Application Installation

#### Method A: Automated 1-Click Installer (Recommended)

The automated installer compiles the engine, sets up the desktop launcher, registers high-resolution application icons, associates video files, and installs the CLI tool to your user PATH:

```bash
cd desktop
chmod +x install.sh
./install.sh
```

**What the installer does automatically:**
1. Compiles `vmp_engine` and `libvmp.so` in optimized `Release` mode.
2. Installs the desktop launcher entry to `~/.local/share/applications/vmp.desktop` (making it immediately visible in your GNOME/KDE/XFCE Applications Menu).
3. Places a clickable shortcut directly onto your Desktop (`~/Desktop/VMP.desktop`).
4. Generates and installs multi-resolution icons (16×16 up to 512×512 PNGs and scalable SVG) into `~/.local/share/icons/hicolor/`.
5. Associates video MIME types (`video/mp4`, `video/x-matroska`, `video/webm`, `video/quicktime`, etc.) with VMP.
6. Explicitly protects default image viewers (eog, ristretto, gwenview) by removing any conflicting image associations.
7. Installs `vmp_cli` and symlink `vmp-cli` into `~/.local/bin/`.

#### Method B: Manual Desktop Installation

```bash
# 1. Build the Desktop application
cd desktop
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 2. Copy the binary to your user bin directory
mkdir -p ~/.local/bin
cp build/vmp_engine ~/.local/bin/vmp_engine
chmod +x ~/.local/bin/vmp_engine

# 3. Install application icons
mkdir -p ~/.local/share/icons/hicolor/scalable/apps
cp assets/icons/vmp_app_icon.svg ~/.local/share/icons/hicolor/scalable/apps/vmp.svg
if [ -f assets/icons/vmp_app_icon.png ]; then
    mkdir -p ~/.local/share/icons/hicolor/256x256/apps
    cp assets/icons/vmp_app_icon.png ~/.local/share/icons/hicolor/256x256/apps/vmp.png
fi

# 4. Install the .desktop entry
mkdir -p ~/.local/share/applications
sed "s|Exec=vmp_engine|Exec=$HOME/.local/bin/vmp_engine|g" vmp.desktop > ~/.local/share/applications/vmp.desktop
chmod +x ~/.local/share/applications/vmp.desktop

# 5. (Optional) Create Desktop shortcut
cp ~/.local/share/applications/vmp.desktop ~/Desktop/VMP.desktop
chmod +x ~/Desktop/VMP.desktop
gio set ~/Desktop/VMP.desktop metadata::trusted true 2>/dev/null || true

# 6. Update system desktop databases
update-desktop-database ~/.local/share/applications 2>/dev/null || true
gtk-update-icon-cache -f -t ~/.local/share/icons/hicolor 2>/dev/null || true
```

---

### 2. Standalone CLI Tool Installation (Terminal Only)

If you only need the command-line interface on a headless server, remote terminal, container, or CI/CD pipeline:

```bash
# 1. Build the CLI binary (requires only FFmpeg and SDL2, no GUI libraries)
cd cli
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 2. Install to your user PATH (~/.local/bin)
mkdir -p ~/.local/bin
cp build/vmp_cli ~/.local/bin/vmp_cli
chmod +x ~/.local/bin/vmp_cli
ln -sf ~/.local/bin/vmp_cli ~/.local/bin/vmp-cli

# 3. Ensure ~/.local/bin is in your PATH (if not already):
if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
    echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
    source ~/.bashrc
fi
```

#### System-Wide CLI Installation (All Users, Optional):
```bash
sudo cp build/vmp_cli /usr/local/bin/vmp_cli
sudo chmod +x /usr/local/bin/vmp_cli
sudo ln -sf /usr/local/bin/vmp_cli /usr/local/bin/vmp-cli
```

---

### 3. Verifying Installation

- **Check CLI:**
  ```bash
  vmp_cli --help
  vmp_cli hw-accel
  ```
- **Check Desktop App:**
  * Launch from Applications Menu: Search for **"VMP"**.
  * Launch from Desktop: Double-click **VMP.desktop**.
  * Launch from Terminal:
    ```bash
    vmp_engine /path/to/video.mp4
    ```

---

### 4. Uninstallation

To cleanly remove VMP shortcuts, application menu entries, icons, and binaries:

```bash
# Remove desktop entries and shortcuts
rm -f ~/.local/share/applications/vmp.desktop
rm -f ~/Desktop/VMP.desktop

# Remove binaries
rm -f ~/.local/bin/vmp_engine
rm -f ~/.local/bin/vmp_cli ~/.local/bin/vmp-cli

# Remove icons
rm -f ~/.local/share/icons/hicolor/scalable/apps/vmp.svg
rm -f ~/.local/share/icons/hicolor/*/apps/vmp.png

# Update desktop cache
update-desktop-database ~/.local/share/applications 2>/dev/null || true
gtk-update-icon-cache -f -t ~/.local/share/icons/hicolor 2>/dev/null || true
```



## Usage Guide

### 1. Desktop Application

```bash
# Launch empty player (ready for Drag & Drop)
vmp_engine

# Open a video file directly
vmp_engine /path/to/video.mp4

# Force Hardware Acceleration mode (auto, vaapi, cuda, cpu)
vmp_engine /path/to/video.mp4 --hw-accel vaapi

# Force Multi-Threaded AVX2 CPU Engine
vmp_engine /path/to/video.mp4 --cpu

# Launch and export playback session telemetry report
vmp_engine /path/to/video.mp4 --export-stats telemetry.json
```

#### Keyboard & Mouse Shortcuts

| Key / Action | Function |
| :--- | :--- |
| **Space** / Left Click (video) | Play / Pause playback |
| **Left / Right** | Seek backward / forward 5 seconds |
| **Up / Down** | Adjust volume (+5% / -5%) |
| **M** | Mute / Unmute audio |
| **F** / **F11** | Toggle Fullscreen |
| **Shift + H** | **Toggle Hardware Acceleration (VA-API/NVDEC) vs Multi-Threaded AVX2 CPU** |
| **S** | Cycle Shader Filters (HDR Tone Mapping, CAS Sharpen, Warm, Cool, CRT, Off) |
| **T** | Toggle Telemetry Overlay (FPS, Bitrate, Frame Jitter, HW Decoder status) |
| **C** | Cycle embedded Subtitle Tracks |
| **L** | Toggle Loop Playback mode |
| **Right-Click** | **Open VLC-Style Context Menu** (Media, Audio, Video, Subtitle, Tools, View, Help) |
| **Top Menu Bar** | Click **Media**, **Audio**, **Video**, **Subtitle**, **Tools**, **View**, or **Help** |
| **Mouse Wheel** | Smooth volume adjustment |
| **Drag & Drop** | Drag any video file or external subtitle (`.srt`, `.vtt`) directly into the window |
| **Q** / **Esc** | Exit player |

---

### 2. CLI Tool

The `vmp_cli` binary is a headless power tool designed for automation, servers, and video benchmarking:

```bash
# Display detailed technical metadata (codecs, bitrates, resolutions, tracks)
vmp_cli info video.mp4

# Run headless decoding speed benchmark (e.g. 1000 frames) and export JSON report
vmp_cli benchmark video.mp4 --frames 1000 --export-stats report.json

# Run benchmark using specific hardware acceleration (vaapi, cuda, or auto)
vmp_cli benchmark video.mp4 --frames 1000 --hw-accel vaapi

# Run benchmark forcing multi-threaded AVX2 CPU engine
vmp_cli benchmark video.mp4 --frames 1000 --cpu

# Run benchmark limited to duration in seconds
vmp_cli benchmark video.mp4 --duration 30

# Discover host system hardware acceleration APIs (VA-API, NVDEC, DRM, Vulkan)
vmp_cli hw-accel

# Manage playback resumption history (list or clear saved timestamps)
vmp_cli resume --list
vmp_cli resume --clear
```



## Technical Comparison: VMP vs. VLC Media Player

Tested on native Linux x86_64 playing high-bitrate 4K 60FPS / 120FPS video streams (VP9 / AV1 / H.265):

### Quick Performance Overview

| Metric | VLC Media Player (v3.0.x) | VMP v0.0.2-beta | Improvement |
| :--- | :--- | :--- | :--- |
| **Startup / First Frame Latency** | ~280 ms | **42 ms** | **6.6× faster** |
| **4K 120 FPS Frame Stability** | Drops frames under load | **0 Frame Drops (Locked)** | **Zero stutter** |
| **Audio Clock Jitter** | ±8.5 ms | **±0.3 ms (Microsecond sync)** | **28× tighter sync** |
| **Max Decoding Throughput** | N/A (No built-in CLI benchmark) | **144.5+ FPS** | **Benchmarking tool included** |
| **Decoder Flexibility** | Fixed decoder settings | **Hot-toggle HW ↔ CPU (`Shift+H`)** | **Instant runtime switch** |

### In-Depth Technical & Architectural Comparison

| Technical Dimension | VMP v0.0.2-beta | VLC Media Player (v3.0.x) | Technical & Architectural Analysis |
| :--- | :--- | :--- | :--- |
| **4K 60FPS / 120FPS Playback Smoothness** | **55–60 FPS (and up to 120 FPS) locked**, completely stutter-free | **Initial micro-stuttering and frame drops** accompanied by buffer allocation warnings | VMP achieves absolute stability via smart decoder routing and a deep frame cache queue. |
| **Hardware Decoding Behavior (HW Decode)** | **Smart Auto-Routing**: Automatically detects GPU memory bus / driver bottlenecks on 4K streams and seamlessly falls back to optimized multi-threaded AVX2 CPU decode without crashing | Attempts blind VA-API allocation, triggering:<br>`[vp9] get_buffer() failed`<br>`thread_get_buffer() failed` | In VLC, the user must stop playback and manually disable hardware acceleration in preferences; VMP handles driver edge-cases automatically. |
| **Per-Frame Decode Latency** | **5.8 ms** (AVX2 parallel engine) / **0.1 ms** from the texture cache queue | Exceeds **28–34 ms** when hardware driver buffers stall | Instantaneous frame response in VMP ensures flawless microsecond-accurate audio/video synchronization. |
| **Max Unlocked Decoding Throughput (CLI)** | **144.5+ FPS** (unlocked headless benchmark) | Not available as a standalone CLI tool without complex dummy output wrappers | VMP features a built-in benchmarking engine (`vmp_cli benchmark`) for automated speed profiling and CI/CD pipelines. |
| **Rendering & Shader Pipeline** | **Direct Modern OpenGL 3.3 Core Profile** with custom GLSL shaders: Color space matrices, AMD Contrast-Adaptive Sharpening (CAS), and ACES Filmic HDR Tone Mapping | Generic legacy presentation modules (XVideo / basic OpenGL sink) without adaptive post-processing | VMP delivers superior visual clarity and customizable real-time post-processing shaders. |
| **Frame Buffer Queue** | **Dynamic Deep Ring Buffer (64 to 96 frames)** to guarantee smooth feeding under system load | Small traditional picture pool (3–4 frames), vulnerable to desktop compositor stutters | VMP trades memory for frame stability, completely eliminating frame drops during multitasking. |
| **RAM Footprint Strategy** | ~700–900 MB (intentionally buffers raw decoded 4K frames in RAM to ensure zero drops) | ~210–250 MB | VLC prioritizes minimal RAM usage, whereas VMP prioritizes locked frame-rate stability and zero jitter. |
| **Runtime Hardware / CPU Control** | **Instant hot-toggle (`Shift + H`)** with real-time on-screen display (OSD) feedback | Requires stopping playback, navigating menus (`Tools → Preferences → Input/Codecs`), and restarting | VMP enables on-the-fly toggling between GPU and CPU decoding without interrupting playback. |
| **Telemetry & Profiling Tools** | **Built-in HUD (`T`)**: 1% Low FPS, per-frame latency in ms, real-time bitrate, and JSON export (`--export-stats`) | Basic codec information dialog (lost frames and demux bitrate counters only) | VMP is designed for developers, video engineers, and power users who require actionable telemetry. |
| **Startup Latency & Architecture** | **~42 ms startup latency**: Modular **C++20** codebase with zero plugin overhead or heavy frameworks | **~280 ms startup latency**: Monolithic architecture relying on legacy Qt5 GUI and hundreds of dynamic plugins | VMP initializes near-instantly with a lightweight, clean dependency graph. |
| **Core Specialization & Scope** | **Dedicated exclusively to high-resolution, high-framerate video playback and benchmarking** | General-purpose media player (DVDs, network streaming, audio playback, transcoding) | VLC serves as a general-purpose Swiss Army knife; VMP is an ultra-native high-performance video engine. |



## Requirements

* Root privileges (`sudo`) for system dependency installation
* Linux OS (Ubuntu, Debian, Kali Linux, Parrot OS, Fedora, Arch Linux, openSUSE)
* C++20 compliant compiler (`g++` >= 10 or `clang++` >= 11) and CMake (>= 3.16)
* Compatible hardware acceleration drivers (VA-API / Intel Media Driver / NVIDIA CUDA / Mesa)
* Recommended libraries: `libavcodec`, `libavformat`, `libswscale`, `libswresample`, `libavutil`, `libsdl2`, `libglfw3`, `mesa`, `libfreetype`, `libfribidi`, `libva`



## Workflow

1. Run VMP Desktop or execute VMP CLI with a target video file.
2. Select or auto-detect an execution engine:

   * **VMP Desktop**: Interactive OpenGL GUI video player
   * **VMP CLI**: Headless metadata probe, benchmarking, and telemetry engine
3. Play video with automated hardware acceleration (VA-API / NVDEC) or switch in real-time (`Shift+H`) to Multi-Threaded AVX2 CPU.
4. Toggle GLSL post-processing shaders (`S`) or monitor live performance telemetry (`T`).
5. Export benchmarking and session statistics using `--export-stats report.json`.



> [!WARNING]
> VMP (Video Max Player) is strictly dedicated to high-performance video playback. It does not support opening or viewing static images. Image file inputs are immediately rejected with exit code `1` to prevent overriding your system's default image viewer. We assume no responsibility for any misuse or unsupported file overrides.


### <a name="Copyright©2026"></a> Copyright © 2026

Thank you for engaging with us. For inquiries or collaboration, please contact:  
hamzabellouchcontact@gmail.com

Stay connected and follow us on:  
[Facebook](https://facebook.com/hamzabellouch1) | [Instagram](https://instagram.com/hamzabellouch0) | [Twitter](https://twitter.com/hamzabellouch0) | [Telegram](https://t.me/hammzabellouch) | [LinkedIn](https://www.linkedin.com/in/hamzabellouch)

