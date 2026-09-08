import os
import sys
import argparse
from PIL import Image, ImageDraw, ImageFont

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
FONT_PATH = os.path.join(BASE_DIR, 'assets/fonts/GoogleSans-Bold.ttf')

PATTERNS = {
    'play': ['play_arrow', 'play'],
    'pause': ['pause'],
    'replay5': ['replay_5', 'replay5', 'replay10', 'replay'],
    'forward5': ['forward_5', 'forward5', 'forward10', 'forward'],
    'fullscreen_exit': ['fullscreen_exit', 'fullscreen-exit', 'fullscreenexit', 'exit_fullscreen'],
    'fullscreen': ['fullscreen'],
    'indicator': ['indicator', 'location_pin', 'pin'],
    'stats': ['stats', 'bar_chart', 'analytics']
}

def create_super_sampled(size=512):
    return Image.new('RGBA', (size, size), (0, 0, 0, 0))

def save_icon(img, name, final_size=128):
    icons_dir = os.path.join(BASE_DIR, 'assets/icons')
    os.makedirs(icons_dir, exist_ok=True)
    out = img.resize((final_size, final_size), Image.Resampling.LANCZOS)
    png_path = os.path.join(icons_dir, f'{name}.png')
    rgba_path = os.path.join(icons_dir, f'{name}.rgba')
    out.save(png_path)
    
    raw_bytes = out.tobytes()
    with open(rgba_path, 'wb') as f:
        f.write(raw_bytes)
    return name, raw_bytes, final_size

def process_custom_icon(path, target_size=128):
    """Processes an image file into a pure-white RGBA icon with sharp alpha channel."""
    img = Image.open(path).convert('RGBA')
    r, g, b, a = img.split()
    
    extrema = a.getextrema()
    # If the image has no transparent pixels, use grayscale luminance as alpha
    if extrema[0] == 255 and extrema[1] == 255:
        gray = img.convert('L')
        alpha_channel = gray.resize((target_size, target_size), Image.Resampling.LANCZOS)
    else:
        alpha_channel = a.resize((target_size, target_size), Image.Resampling.LANCZOS)
        
    white = Image.new('L', (target_size, target_size), 255)
    return Image.merge('RGBA', (white, white, white, alpha_channel))

def find_custom_icon(icons_dir, key):
    if not icons_dir or not os.path.isdir(icons_dir):
        return None
    candidates = PATTERNS.get(key, [key])
    files = sorted(os.listdir(icons_dir))
    
    # 1. Exact base match first (e.g. play.png -> play)
    for f in files:
        base, ext = os.path.splitext(f)
        if ext.lower() in ('.png', '.jpg', '.jpeg', '.webp'):
            if base.lower() == key.lower():
                return os.path.join(icons_dir, f)
                
    # 2. Pattern match in priority order
    for pattern in candidates:
        for f in files:
            base, ext = os.path.splitext(f)
            if ext.lower() in ('.png', '.jpg', '.jpeg', '.webp'):
                if pattern in base.lower():
                    # Ensure fullscreen doesn't match fullscreen_exit
                    if key == 'fullscreen' and 'exit' in base.lower():
                        continue
                    return os.path.join(icons_dir, f)
    return None

def draw_play():
    img = create_super_sampled(512)
    draw = ImageDraw.Draw(img)
    pts = [(160, 112), (400, 256), (160, 400)]
    draw.polygon(pts, fill=(255, 255, 255, 255))
    return save_icon(img, 'play', 128)

def draw_pause():
    img = create_super_sampled(512)
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle([130, 112, 215, 400], radius=20, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([297, 112, 382, 400], radius=20, fill=(255, 255, 255, 255))
    return save_icon(img, 'pause', 128)

def draw_fullscreen():
    img = create_super_sampled(512)
    draw = ImageDraw.Draw(img)
    w = 36
    length = 130
    draw.rounded_rectangle([80, 80, 80 + length, 80 + w], radius=10, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([80, 80, 80 + w, 80 + length], radius=10, fill=(255, 255, 255, 255))
    
    draw.rounded_rectangle([432 - length, 80, 432, 80 + w], radius=10, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([432 - w, 80, 432, 80 + length], radius=10, fill=(255, 255, 255, 255))
    
    draw.rounded_rectangle([80, 432 - w, 80 + length, 432], radius=10, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([80, 432 - length, 80 + w, 432], radius=10, fill=(255, 255, 255, 255))
    
    draw.rounded_rectangle([432 - length, 432 - w, 432, 432], radius=10, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([432 - w, 432 - length, 432, 432], radius=10, fill=(255, 255, 255, 255))
    return save_icon(img, 'fullscreen', 128)

def draw_fullscreen_exit():
    img = create_super_sampled(512)
    draw = ImageDraw.Draw(img)
    w = 36
    length = 130
    draw.rounded_rectangle([80, 210 - w, 80 + length, 210], radius=10, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([210 - w, 80, 210, 80 + length], radius=10, fill=(255, 255, 255, 255))
    
    draw.rounded_rectangle([432 - length, 210 - w, 432, 210], radius=10, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([302, 80, 302 + w, 80 + length], radius=10, fill=(255, 255, 255, 255))
    
    draw.rounded_rectangle([80, 302, 80 + length, 302 + w], radius=10, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([210 - w, 432 - length, 210, 432], radius=10, fill=(255, 255, 255, 255))
    
    draw.rounded_rectangle([432 - length, 302, 432, 302 + w], radius=10, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([302, 432 - length, 302 + w, 432], radius=10, fill=(255, 255, 255, 255))
    return save_icon(img, 'fullscreen_exit', 128)

def draw_stats():
    img = create_super_sampled(512)
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle([100, 240, 180, 412], radius=15, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([216, 120, 296, 412], radius=15, fill=(255, 255, 255, 255))
    draw.rounded_rectangle([332, 180, 412, 412], radius=15, fill=(255, 255, 255, 255))
    return save_icon(img, 'stats', 128)

def draw_indicator():
    img = create_super_sampled(512)
    draw = ImageDraw.Draw(img)
    draw.ellipse([128, 64, 384, 320], fill=(0, 216, 255, 255))
    pts = [(128, 200), (384, 200), (256, 448)]
    draw.polygon(pts, fill=(0, 216, 255, 255))
    draw.ellipse([184, 120, 328, 264], fill=(255, 255, 255, 255))
    draw.ellipse([220, 156, 292, 228], fill=(0, 180, 220, 255))
    return save_icon(img, 'indicator', 128)

def draw_replay5():
    existing_png = os.path.join(BASE_DIR, 'assets/icons/replay5.png')
    if os.path.exists(existing_png):
        img = Image.open(existing_png).convert('RGBA')
        return save_icon(img, 'replay5', 128)
    img = create_super_sampled(512)
    return save_icon(img, 'replay5', 128)

def draw_forward5():
    existing_png = os.path.join(BASE_DIR, 'assets/icons/forward5.png')
    if os.path.exists(existing_png):
        img = Image.open(existing_png).convert('RGBA')
        return save_icon(img, 'forward5', 128)
    img = create_super_sampled(512)
    return save_icon(img, 'forward5', 128)

def load_or_draw_icon(name, fallback_draw_fn, icons_dir=None, final_size=128):
    custom_path = find_custom_icon(icons_dir, name)
    if custom_path:
        print(f"  [+] Loading custom icon '{name}' from: {custom_path}")
        img = process_custom_icon(custom_path, final_size)
        return save_icon(img, name, final_size)
    else:
        print(f"  [.] Generating built-in design for '{name}'")
        return fallback_draw_fn()

def generate_all(icons_dir=None):
    print(f"Generating VMP UI icons (custom directory: {icons_dir})...")
    
    icon_generators = [
        ('play', draw_play),
        ('pause', draw_pause),
        ('replay5', draw_replay5),
        ('forward5', draw_forward5),
        ('fullscreen', draw_fullscreen),
        ('fullscreen_exit', draw_fullscreen_exit),
        ('indicator', draw_indicator),
        ('stats', draw_stats)
    ]
    
    generated_icons = {}
    for name, fn in icon_generators:
        res = load_or_draw_icon(name, fn, icons_dir, 128)
        generated_icons[name] = res
        
    # Write src/material_icons_data.h
    header_path = os.path.join(BASE_DIR, 'src/material_icons_data.h')
    with open(header_path, 'w') as f:
        f.write("#ifndef OCULUS_MATERIAL_ICONS_DATA_H\n#define OCULUS_MATERIAL_ICONS_DATA_H\n\n")
        f.write("#include <cstdint>\n\n")
        f.write("constexpr int ICON_SIZE = 128;\n\n")
        
        # Primary UI Icons in material_icons_data.h
        for name in ['play', 'pause', 'fullscreen', 'fullscreen_exit', 'indicator', 'stats']:
            _, data, _ = generated_icons[name]
            f.write(f"static const uint8_t ICON_{name.upper()}_RGBA[] = {{\n")
            hex_items = [f"0x{b:02x}" for b in data]
            lines = [", ".join(hex_items[i:i+16]) for i in range(0, len(hex_items), 16)]
            f.write(",\n".join(lines))
            f.write("\n};\n\n")
            
        f.write("#endif // OCULUS_MATERIAL_ICONS_DATA_H\n")
    print(f"Generated C++ Header: {header_path}")
    print(f"Generated C++ Header: {header_path}")
    
    # Write src/seek_icons_data.h
    seek_header_path = os.path.join(BASE_DIR, 'src/seek_icons_data.h')
    with open(seek_header_path, 'w') as f:
        f.write("#ifndef OCULUS_SEEK_ICONS_DATA_H\n#define OCULUS_SEEK_ICONS_DATA_H\n\n")
        f.write("#include <cstdint>\n\n")
        for name in ['replay5', 'forward5']:
            _, data, _ = generated_icons[name]
            f.write(f"static const uint8_t ICON_{name.upper()}_RGBA[] = {{\n")
            hex_items = [f"0x{b:02x}" for b in data]
            lines = [", ".join(hex_items[i:i+16]) for i in range(0, len(hex_items), 16)]
            f.write(",\n".join(lines))
            f.write("\n};\n\n")
        f.write("#endif // OCULUS_SEEK_ICONS_DATA_H\n")
    print(f"Generated C++ Header: {seek_header_path}")
    print("Done generating all icon assets and headers!")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Generate VMP UI icon assets and C++ data headers.")
    parser.add_argument('--icons-dir', type=str, default=None, help="Directory containing custom PNG icons (e.g. ./assets/custom_icons)")
    args = parser.parse_args()
    
    target_dir = args.icons_dir
    if target_dir is None:
        default_candidate = os.path.join(BASE_DIR, 'assets/custom_icons')
        if os.path.isdir(default_candidate):
            target_dir = default_candidate
            
    generate_all(target_dir)

