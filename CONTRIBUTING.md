# Contributing

Thank you for your interest in contributing to **VMP**! VMP (Video Max Player) is an ultra-native, high-performance video player and benchmarking suite for Linux OS, engineered in modern **C++20** with OpenGL 3.3, SDL2, FFmpeg, and hardware acceleration (VA-API / NVDEC).

Before submitting a bug report or feature request, please search existing issues (including closed ones) to ensure it hasn't already been reported or discussed. If there are no duplicates, feel free to submit a new issue using the appropriate template.

**Please note:** Issues that do not use existing templates or lack sufficient detail may be closed without review.

For questions, feedback, or collaboration, you can reach out via official email: hamzabellouchcontact@gmail.com or [Social Media platforms](https://sites.google.com/view/hamzabellouch/).


## Disclaimer

VMP is an active Linux media engineering project focused on delivering high-framerate, zero-drop video playback, microsecond-accurate audio synchronization, real-time GLSL post-processing shaders, and deep telemetry benchmarking. While we strive for absolute code quality and reliability, contributions and feedback are always welcome to improve performance and user experience.



## Bug Reports

When submitting a bug report, please make sure your issue contains **sufficient information** to reproduce the problem. Useful details include:

- Linux distribution, desktop environment (GNOME, KDE Plasma, XFCE, Wayland/X11), and kernel version (`uname -r`)
- GPU model and installed graphics driver version (e.g., Mesa Intel/Radeon VA-API, NVIDIA proprietary driver)
- VMP application version (`vmp_engine --version` or `vmp_cli --version`)
- Target media file technical specs (codec, resolution, bitrate, container) or a minimal reproducible sample
- Exact steps to reproduce the bug
- Terminal logs, GDB backtrace, or AddressSanitizer output if applicable



## Feature Requests

VMP aims to remain an ultra-fast, lightweight, strictly video-focused media player and benchmarking engine for Linux. We welcome suggestions that enhance playback smoothness, hardware decoder compatibility, shader effects, audio synchronization, or CLI telemetry metrics.

When suggesting a new feature, please consider:
- **Relevance:** Does the feature fit the core goal of high-performance video playback or benchmarking? (Note: VMP is strictly video-only; static image viewer features will be rejected).
- **Native Performance & Simplicity:** We prioritize modern C++20 standard libraries and lightweight native implementations over heavy external dependencies or web/Electron frameworks.
- **System Stability:** Does the change maintain seamless fallback between GPU hardware acceleration and multi-threaded AVX2 CPU decoding?



## Pull Requests

If you wish to contribute directly by submitting code:

1. **Discuss First:** Leave a comment under an existing issue or open a new issue describing the changes you plan to make before writing code.
2. **Avoid Conflicts:** Comment on the issue to let others know you are working on it to prevent duplicate efforts.
3. **Follow Code Style:** Ensure your code follows modern **C++20** conventions, clean RAII principles, zero memory leaks, and clear commenting.
4. **Verify Both Targets:** Ensure your changes build and run cleanly across both sub-projects:
   - Headless CLI target (`cli/`)
   - Desktop GUI application (`desktop/`)



## New Contributors

If you are new to the project:
- Browse our open issues for beginner-friendly tasks marked as `good first issue` or `help wanted`.
- Feel free to ask clarifying questions directly on the issue thread!



## Building From Source

To build VMP locally on Linux:

1. **Prerequisites:**
   - C++20 compliant compiler (`g++` >= 10 or `clang++` >= 11)
   - CMake (>= 3.16) and `pkg-config`
   - Media libraries: FFmpeg (`libavcodec`, `libavformat`, `libswscale`, `libswresample`, `libavutil`), SDL2, GLFW3, OpenGL, FreeType, FriBidi, libva

2. **Steps:**

   **Install Dependencies (Debian / Ubuntu / Kali / Mint):**
   ```bash
   sudo apt update
   sudo apt install -y build-essential cmake pkg-config \
                       libavcodec-dev libavformat-dev libswscale-dev \
                       libswresample-dev libavutil-dev \
                       libgl1-mesa-dev libglfw3-dev libsdl2-dev \
                       libfreetype-dev libfribidi-dev libva-dev
   ```

   **Clone and Build:**
   ```bash
   # Clone the repository
   git clone https://github.com/hamzabellouch/vmp.git
   cd vmp

   # Build all targets (Unified Release build)
   cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(nproc)
   ```

   **Run:**
   ```bash
   # Desktop GUI Player
   ./build/desktop/vmp_engine /path/to/video.mp4

   # CLI Tool
   ./build/cli/vmp_cli info /path/to/video.mp4
   ./build/cli/vmp_cli benchmark /path/to/video.mp4 --frames 1000
   ```
