================================================================================
           VMP SUITE v2.0 - Ultra-Native Linux Video Media Player
================================================================================

VMP (Video Media Player) is a high-performance C++20 media engine designed
specifically for Linux, delivering ultra-fast playback of high-bitrate videos
(up to 4K/8K/16K) with microsecond-accurate audio/video synchronization,
hardware decoding, VLC-style interface, and custom GLSL post-processing shaders.
No web views or Electron overhead.

IMPORTANT NOTE:
  VMP is engineered EXCLUSIVELY FOR VIDEO PLAYBACK.
  Image formats (.png, .jpg, .bmp, .svg, .webp, etc.) are intentionally rejected
  at startup with exit code 1 to avoid overriding your desktop's default image viewer.

================================================================================
1. DIRECTORY STRUCTURE
================================================================================

The repository is organized into two self-contained sub-projects:

  VMP/
  ├── cli/                 [Directory 1]: Command-Line Interface (Headless)
  │   ├── CMakeLists.txt   Standalone build file (zero OpenGL/GLFW dependencies)
  │   ├── README.md        Documentation for CLI tool
  │   └── src/             Source files for CLI, probe, benchmark, and HW accel
  │
  ├── desktop/             [Directory 2]: Desktop GUI Application
  │   ├── CMakeLists.txt   Standalone build file for GUI and shared library
  │   ├── README.md        Documentation and keyboard shortcuts
  │   ├── install.sh       Desktop installation script (app menu & desktop icon)
  │   ├── vmp.desktop      Linux desktop entry file (video MIME types only)
  │   ├── assets/          Icons and visual assets
  │   ├── generate_icons.py Icon generation script
  │   └── src/             Source files for OpenGL, GLFW, shaders, and UI
  │
  ├── CMakeLists.txt       Root orchestrator build file
  ├── README.md            Markdown documentation (Linux installation & guide)
  └── README.txt           Plain text documentation (English)

================================================================================
2. DEPENDENCIES & REQUIREMENTS
================================================================================

Debian / Ubuntu / Kali / Linux Mint:
  sudo apt update
  sudo apt install -y build-essential cmake pkg-config \
                      libavcodec-dev libavformat-dev libswscale-dev \
                      libswresample-dev libavutil-dev \
                      libgl1-mesa-dev libglfw3-dev libsdl2-dev \
                      libfreetype-dev libfribidi-dev libva-dev

Fedora / RHEL / CentOS:
  sudo dnf install -y gcc-c++ cmake pkgconf \
                      ffmpeg-free-devel SDL2-devel glfw-devel \
                      mesa-libGL-devel freetype-devel fribidi-devel libva-devel

Arch Linux / Manjaro:
  sudo pacman -S --needed base-devel cmake pkgconf ffmpeg sdl2 glfw-x11 mesa freetype2 fribidi libva

================================================================================
3. BUILD INSTRUCTIONS
================================================================================

Option A: Unified Build from Root (Builds both CLI and Desktop)
--------------------------------------------------------------
  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j$(nproc)

  Outputs:
    - build/cli/vmp_cli
    - build/desktop/vmp_engine (and vmp, vmp_desktop aliases)
    - build/desktop/libvmp.so

Option B: Build CLI Only (Standalone, Headless, Fast)
----------------------------------------------------
  cd cli
  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j$(nproc)

  Output:
    - cli/build/vmp_cli

Option C: Build Desktop App Only (Standalone)
--------------------------------------------
  cd desktop
  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j$(nproc)

  Outputs:
    - desktop/build/vmp_engine
    - desktop/build/libvmp.so

================================================================================
4. INSTALLATION ON LINUX
================================================================================

[DESKTOP APPLICATION]
  Method 1: Automated 1-Click Installer (Recommended)
    cd desktop
    chmod +x install.sh
    ./install.sh

    What it does:
      - Compiles vmp_engine and libvmp.so in Release mode.
      - Registers ~/.local/share/applications/vmp.desktop for Applications Menu.
      - Places shortcut on ~/Desktop/VMP.desktop.
      - Installs icons (16px - 512px & SVG) into ~/.local/share/icons/hicolor/.
      - Configures video MIME associations and protects default image viewers.
      - Installs vmp_cli into ~/.local/bin/.

  Method 2: Manual Installation
    cd desktop && cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
    mkdir -p ~/.local/bin ~/.local/share/applications ~/.local/share/icons/hicolor/scalable/apps
    cp build/vmp_engine ~/.local/bin/vmp_engine
    cp assets/icons/vmp_app_icon.svg ~/.local/share/icons/hicolor/scalable/apps/vmp.svg
    sed "s|Exec=vmp_engine|Exec=$HOME/.local/bin/vmp_engine|g" vmp.desktop > ~/.local/share/applications/vmp.desktop

[CLI TOOL INSTALLATION]
  cd cli
  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
  mkdir -p ~/.local/bin
  cp build/vmp_cli ~/.local/bin/vmp_cli
  chmod +x ~/.local/bin/vmp_cli
  ln -sf ~/.local/bin/vmp_cli ~/.local/bin/vmp-cli

================================================================================
5. UNINSTALLATION
================================================================================

  rm -f ~/.local/share/applications/vmp.desktop
  rm -f ~/Desktop/VMP.desktop
  rm -f ~/.local/bin/vmp_engine ~/.local/bin/vmp_cli ~/.local/bin/vmp-cli
  rm -f ~/.local/share/icons/hicolor/scalable/apps/vmp.svg
  rm -f ~/.local/share/icons/hicolor/*/apps/vmp.png
  update-desktop-database ~/.local/share/applications 2>/dev/null || true

================================================================================
6. KEYBOARD & MOUSE SHORTCUTS (DESKTOP)
================================================================================

  Space / Click   Play / Pause playback
  Left / Right    Seek backward / forward 5 seconds
  Up / Down       Adjust volume (+5% / -5%)
  M               Mute / Unmute audio
  F / F11         Toggle Fullscreen mode
  Shift + H       Toggle Hardware Acceleration vs Multi-Threaded AVX2 CPU
  S               Cycle Shaders (HDR, CAS Sharpen, Warm, Cool, CRT, Off)
  T               Toggle Telemetry Overlay (FPS, Bitrate, Jitter, HW Decoder)
  C               Cycle embedded Subtitle Tracks
  L               Toggle Loop Playback
  Right Click     Open VLC-Style Context Menu
  Top Bar         Media, Audio, Video, Subtitle, Tools, View, Help menus
  Mouse Wheel     Volume adjustment
  Drag & Drop     Drop video or .srt files directly into window
  Q / Esc         Quit player

================================================================================
7. CLI TOOL USAGE
================================================================================

  vmp_cli info <video.mp4>
      Print technical container and codec streams.

  vmp_cli benchmark <video.mp4> [options]
      Speed benchmark. Options:
        --frames <N>         Limit to N frames
        --duration <sec>     Limit to duration in seconds
        --hw-accel <mode>    vaapi, cuda, or auto
        --cpu                Force multi-threaded AVX2 CPU engine
        --export-stats <f>   Export JSON report

  vmp_cli hw-accel
      Probe and print host hardware decoding capabilities.

  vmp_cli resume --list / --clear
      Inspect or clear playback timestamp resumption history.
