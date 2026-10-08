Dear ImGui Web UI Implementation

This project implements a web-based Dear ImGui UI that matches the desktop AEC Client UI, using pure JavaScript to replicate Dear ImGui patterns and behavior.

## Architecture

- **Python Backend** (web/server.py, web/chain.py): Same as desktop, provides the audio processing and API
- **Web Frontend** (web/ui/imgui_web.html): Dear ImGui-style UI implemented in pure JavaScript
- **UI Logic** (web/ui/imgui_web.js): JavaScript implementation of Dear ImGui UI patterns and controls
- **Styling** (web/ui/imgui_web.css): CSS matching the desktop UI theme

**Note**: This is a DOM implementation (real selects, sliders, checkboxes
and buttons) styled to mirror the desktop Dear ImGui layout — same
sections, labels, tab bar and footer. No canvas, no WebAssembly, so
every control is clickable. Same PROFILES table as the desktop
(DTLN-AEC 128 / WebRTC AEC3 / NKF-AEC) with the same
experimental-engines gating; options whose backend is missing are
disabled with a note. Fixed at 16 kHz like the desktop's neural
engines.

## Key Features

### Desktop UI Match
- Tab-based navigation (Audio, Appearance, About)
- Modern card-based layout with rounded corners
- Professional dark theme with accent colors
- Custom widgets (status dots, level meters, inline dots)
- Live status indicators and telemetry
- Profile selection system
- Wallpaper support

### Same Functionality
- Device enumeration and selection (devices locked while running)
- Microphone level 0–200% (More → Levels, like the desktop)
- Noise suppression toggle (live) — DTLN-NS post stage on every engine
- Profile section (3 engines, hot-swappable while running; extras hidden until experimental tick)
- NKF telemetry line (delay lock, exposure, backstop/loop/RES trim, guard resets)
- Start/Stop + Reset to defaults, with live status
- Live level meters (Mic/Out), while running
- Minimize to system tray (native window only)
- Wallpaper picker (browser-side backdrop, remembered in localStorage)

### Live Controls
- NS toggle applies immediately
- Profile switches hot-swap the engine (streams untouched, new engine starts reset)
- Mic gain applies live to running chain
- Device and Listen-to-myself changes require Stop -> Start
- All status updates via WebSocket

## Setup

1. Ensure Python environment is set up:
   ```bash
   python -m venv .venv
   .venv\Scripts\activate
   pip install -r web/requirements.txt
   ```

2. Run the web server:
   ```bash
   python -m web.server
   ```

3. Open in browser:
   ```
   http://localhost:8000
   ```

The UI loads as plain DOM + CSS/JS with no build step and no
WebAssembly.

## Files

### web/ui/
- `imgui_web.html` - Main UI page (loads `imgui_web.js`, shows real boot errors)
- `imgui_web.js` - DOM UI mirroring src/main.cpp DrawUI sections and footer
- `imgui_web.css` - Dear ImGui dark theme (horizontal tab bar, 190px label rows)
- `index.html` - Classic fallback UI at `/classic`

### web/server.py
- Modified to serve the new UI from web/ui/imgui_web.html
- Added WebSocket endpoint for UI state updates
- Maintains all existing audio processing logic

### web/gui.py
- No changes needed - pywebview will load the new UI automatically

## Benefits

1. **Consistency**: Exact visual match to desktop UI
2. **No Antivirus Warnings**: Pure Python, no .exe files
3. **Feature Parity**: All desktop functionality available
4. **Modern Look**: Professional UI with smooth interactions
5. **Performance**: Same audio processing as desktop, just web-based UI
6. **Cross-Platform**: Works on any platform with pywebview support
7. **Low Maintenance**: Reuses all existing backend logic
8. **Universal Compatibility**: Works in all modern browsers without WebAssembly

## Limitations

1. **Browser Dependencies**: Requires modern browser with Web Workers support
2. **Graphics**: Limited 2D/3D capabilities vs native OpenGL
3. **System Integration**: Some desktop features (tray, wallpaper) may have limitations
4. **Performance**: Generally acceptable for real-time audio processing

## Usage

The web UI provides the same features as the desktop version but in a browser context:

1. Same device selection and configuration
2. Same real-time processing controls
3. Same telemetry and status display
4. Same profile system
5. Same minimize-to-tray functionality (via pywebview)

## License

MIT License. Source: https://github.com/samudinzul/aec-client
