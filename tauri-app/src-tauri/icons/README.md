# App Icons

This directory must contain the icons listed in `tauri.conf.json`:

```
icons/
  32x32.png          # taskbar / dock small
  128x128.png        # desktop
  128x128@2x.png     # retina desktop (256x256 physical pixels)
  icon.icns          # macOS bundle icon (multi-size container)
  icon.ico           # Windows installer / taskbar icon
```

## Generating icons from a source image

Run the Tauri CLI icon generator:

```bash
# From tauri-app/
npx tauri icon /path/to/source-icon.png
```

This command generates all required sizes and formats from a single 1024x1024 PNG.
The generated files are placed in `src-tauri/icons/` automatically.

## Quick placeholder (for CI / dev builds without a real icon)

```bash
# From repo root:
python3 tools/gen-icons.py
```

This generates minimal solid-color PNG files (pokéball red `#CC0000`) suitable
for test builds. Replace with a proper icon before distributing.
