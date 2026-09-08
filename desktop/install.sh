#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Installing VMP (Video Media Player) for Linux Desktop ==="

# 1. Build if not already built
mkdir -p build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 2. Create required standard directories
mkdir -p "$HOME/.local/share/applications"
mkdir -p "$HOME/.local/share/icons/hicolor/scalable/apps"
mkdir -p "$HOME/.local/share/icons/hicolor/128x128/apps"
mkdir -p "$HOME/.local/share/icons/hicolor/256x256/apps"
mkdir -p "$HOME/Desktop"
# Clean up any legacy Oculus shortcuts or icons
rm -f "$HOME/.local/share/applications/oculus.desktop"
rm -f "$HOME/Desktop/oculus.desktop"
rm -f "$HOME/Desktop/Oculus.desktop"
rm -f "$HOME/.local/bin/oculus_cli" "$HOME/.local/bin/oculus-cli"
rm -f "$HOME/.local/share/icons/hicolor/scalable/apps/oculus.svg"

# 3. Copy application icon
ICON_SVG="$SCRIPT_DIR/assets/icons/vmp_app_icon.svg"
ICON_PNG="$SCRIPT_DIR/assets/icons/vmp_app_icon.png"

if [ -f "$ICON_SVG" ]; then
    cp -f "$ICON_SVG" "$HOME/.local/share/icons/hicolor/scalable/apps/vmp.svg"
fi
for sz in 16 24 32 48 64 128 256 512; do
    mkdir -p "$HOME/.local/share/icons/hicolor/${sz}x${sz}/apps"
    rm -f "$HOME/.local/share/icons/hicolor/${sz}x${sz}/apps/oculus.png"
    if [ -f "$ICON_SVG" ] && command -v magick &> /dev/null; then
        magick -background none "$ICON_SVG" -resize "${sz}x${sz}" "$HOME/.local/share/icons/hicolor/${sz}x${sz}/apps/vmp.png"
    elif [ -f "$ICON_PNG" ]; then
        cp -f "$ICON_PNG" "$HOME/.local/share/icons/hicolor/${sz}x${sz}/apps/vmp.png"
    fi
done

# 4. Configure and install .desktop file
DESKTOP_SRC="$SCRIPT_DIR/vmp.desktop"
DESKTOP_TARGET="$HOME/.local/share/applications/vmp.desktop"
DESKTOP_SHORTCUT="$HOME/Desktop/VMP.desktop"

sed "s|Exec=vmp_engine|Exec=$SCRIPT_DIR/build/vmp_engine|g" "$DESKTOP_SRC" > "$DESKTOP_TARGET"
chmod +x "$DESKTOP_TARGET"

cp "$DESKTOP_TARGET" "$DESKTOP_SHORTCUT"
chmod +x "$DESKTOP_SHORTCUT"
# Trust desktop file in GNOME/KDE if gio is present
if command -v gio &> /dev/null; then
    gio set "$DESKTOP_SHORTCUT" metadata::trusted true 2>/dev/null || true
fi

# 5. Install CLI tool (if built)
CLI_BIN="$SCRIPT_DIR/../cli/build/vmp_cli"
if [ -f "$CLI_BIN" ]; then
    mkdir -p "$HOME/.local/bin"
    cp -f "$CLI_BIN" "$HOME/.local/bin/vmp_cli"
    chmod +x "$HOME/.local/bin/vmp_cli"
    ln -sf "$HOME/.local/bin/vmp_cli" "$HOME/.local/bin/vmp-cli"
fi

# 6. Update desktop database
if command -v update-desktop-database &> /dev/null; then
    update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
fi
if command -v gtk-update-icon-cache &> /dev/null; then
    gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
fi

# 7. Disassociate VMP from all image formats (prevent overriding default image viewer)
for img_mime in image/png image/jpeg image/jpg image/bmp image/svg+xml image/webp image/gif image/tiff; do
    if command -v xdg-mime &> /dev/null; then
        curr_def=$(xdg-mime query default "$img_mime" 2>/dev/null || true)
        if [ "$curr_def" = "vmp.desktop" ] || [ "$curr_def" = "VMP.desktop" ] || [ "$curr_def" = "oculus.desktop" ] || [ "$curr_def" = "Oculus.desktop" ]; then
            for v in org.xfce.ristretto.desktop ristretto.desktop org.gnome.eog.desktop eog.desktop gwenview.desktop; do
                if [ -f "/usr/share/applications/$v" ]; then
                    xdg-mime default "$v" "$img_mime" 2>/dev/null || true
                    break
                fi
            done
        fi
    fi
done

MIMEAPPS="$HOME/.config/mimeapps.list"
if [ -f "$MIMEAPPS" ]; then
    if ! grep -q "\[Removed Associations\]" "$MIMEAPPS"; then
        echo -e "\n[Removed Associations]" >> "$MIMEAPPS"
    fi
    for img_mime in image/png image/jpeg image/jpg image/bmp image/svg+xml image/webp image/gif image/tiff; do
        if ! grep -q "^$img_mime=.*vmp\.desktop" "$MIMEAPPS"; then
            echo "$img_mime=vmp.desktop;" >> "$MIMEAPPS"
        fi
        sed -i "/^$img_mime=.*oculus\.desktop/d" "$MIMEAPPS" 2>/dev/null || true
    done
fi

echo "=========================================================="
echo "✔ VMP (Video Media Player) successfully installed as Desktop App!"
echo "✔ Applications Menu shortcut: ~/.local/share/applications/vmp.desktop"
echo "✔ Desktop shortcut: ~/Desktop/VMP.desktop"
echo "✔ CLI tool installed: ~/.local/bin/vmp_cli (and vmp-cli)"
echo "✔ Run Desktop: $SCRIPT_DIR/build/vmp_engine"
echo "✔ Run CLI:     ~/.local/bin/vmp_cli --help"
echo "=========================================================="
