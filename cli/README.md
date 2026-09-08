# VMP CLI v0.0.2-beta - Ultra-Native Video Max Player CLI

A lightweight, high-performance standalone command-line tool for video probing, headless decoding speed benchmarks, hardware acceleration discovery, and playback resume management without requiring a graphical desktop environment.

> ⚠️ **Important:** VMP CLI is strictly for video streams and containers. Image files are rejected with exit code 1.

---

## 📦 Requirements & Dependencies

The CLI has **zero GUI dependencies** (no OpenGL, GLFW, FreeType, or FriBidi required).

### Ubuntu / Debian / Kali / Linux Mint:
```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
                    libavcodec-dev libavformat-dev libswscale-dev \
                    libswresample-dev libavutil-dev libsdl2-dev libva-dev
```

### Fedora / RHEL / CentOS:
```bash
sudo dnf install -y gcc-c++ cmake pkgconf \
                    ffmpeg-free-devel SDL2-devel libva-devel
```

### Arch Linux / Manjaro:
```bash
sudo pacman -S --needed base-devel cmake pkgconf ffmpeg sdl2 libva
```

---

## 🛠️ Build Instructions (Standalone Headless)

```bash
cd cli
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Generated binary:
- `build/vmp_cli`

---

## 📥 Installation on Linux

### 1. User Installation (Recommended: `~/.local/bin`)

```bash
mkdir -p ~/.local/bin
cp build/vmp_cli ~/.local/bin/vmp_cli
chmod +x ~/.local/bin/vmp_cli
ln -sf ~/.local/bin/vmp_cli ~/.local/bin/vmp-cli

# Verify PATH configuration:
if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
    echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
    source ~/.bashrc
fi
```

### 2. System-Wide Installation (`/usr/local/bin`)

```bash
sudo cp build/vmp_cli /usr/local/bin/vmp_cli
sudo chmod +x /usr/local/bin/vmp_cli
sudo ln -sf /usr/local/bin/vmp_cli /usr/local/bin/vmp-cli
```

---

## 🔍 Commands & Usage Guide

### 1. Probe Video Metadata (`info`)
```bash
vmp_cli info movie.mp4
```
Displays container format, bitrates, duration, stream details (video codecs, resolution, FPS, pixel format; audio codecs, sample rate, channels, languages; subtitle tracks).

### 2. Headless Decoding Benchmark (`benchmark`)
```bash
# Run benchmark on 1000 frames with auto HW acceleration
vmp_cli benchmark movie.mp4 --frames 1000 --export-stats report.json

# Force VA-API or CUDA hardware decoding
vmp_cli benchmark movie.mp4 --frames 1000 --hw-accel vaapi

# Force multi-threaded AVX2 CPU engine
vmp_cli benchmark movie.mp4 --frames 1000 --cpu

# Limit benchmark to duration in seconds
vmp_cli benchmark movie.mp4 --duration 30
```

Benchmark output metrics:
- **Average Decoding FPS**
- **1% Low FPS**
- **Average frame latency (ms)**
- **Hardware decoder probe status**
- **Comprehensive JSON telemetry export**

### 3. Hardware Acceleration Discovery (`hw-accel`)
```bash
vmp_cli hw-accel
```
Scans the host system and lists available hardware acceleration decoders (VAAPI, NVDEC/CUDA, DRM, VDPAU, QSV, Vulkan).

### 4. Playback Resume History (`resume`)
```bash
# List all remembered video positions
vmp_cli resume --list

# Clear saved playback positions
vmp_cli resume --clear
```

---

## 🗑️ Uninstallation

```bash
rm -f ~/.local/bin/vmp_cli ~/.local/bin/vmp-cli
# Or if installed system-wide:
sudo rm -f /usr/local/bin/vmp_cli /usr/local/bin/vmp-cli
```
