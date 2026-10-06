Dear ImGui Web UI Implementation

This project implements a web-based Dear ImGui UI that matches the desktop AEC Client UI, using pure JavaScript to replicate Dear ImGui patterns and behavior.

## Architecture

- **Python Backend** (web/server.py, web/chain.py): Same as desktop, provides the audio processing and API
- **Web Frontend** (web/ui/imgui_web.html): Dear ImGui-style UI implemented in pure JavaScript
- **UI Logic** (web/ui/imgui_web.js): JavaScript implementation of Dear ImGui UI patterns and controls
- **Styling** (web/ui/imgui_web.css): CSS matching the desktop UI theme

**Note**: This implementation uses pure JavaScript to replicate Dear ImGui UI patterns and behavior, providing a native-like experience without requiring WebAssembly. The UI has the same visual polish, responsive behavior, and feature parity as the desktop version.

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
- Device enumeration and selection
- Microphone gain control (-12dB to +12dB)
- Noise suppression toggle
- Feedback notch toggle
- Start/Stop button with live status
- Level meters for Mic/Ref/Out
- Live level telemetry
- Minimize to system tray
- Experimental engines toggle
- Wallpapers (if available)

### Live Controls
- NS and notch toggles apply immediately
- Mic gain applies live to running chain
- Device changes require restart
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
   python web/server.py
   ```

3. Open in browser:
   ```
   http://localhost:8000
   ```

The UI will load using the pure JavaScript Dear ImGui implementation and provide the same experience as the desktop version. No WebAssembly compilation required!

## Files

### web/ui/
- `imgui_web.html` - Main UI page with WebAssembly integration
- `imgui_web.js` - JavaScript wrapper for Dear ImGui
- `imgui_web.css` - CSS styling matching desktop UI
- `imgui_wrapper.js` - Python-WebAssembly communication bridge

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
