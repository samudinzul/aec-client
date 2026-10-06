// Pure JavaScript Dear ImGui Web UI Implementation
// No WebAssembly required - works in all modern browsers

class WebImGuiUI {
    constructor() {
        this.canvas = null;
        this.ctx = null;
        this.mousePos = { x: 0, y: 0 };
        this.mouseDown = [false, false, false];
        this.mouseWheel = 0;
        this.keysDown = {};
        this.lastTime = 0;
        this.fps = 0;
        
        // UI State
        this.tab = 'audio';
        this.isRunning = false;
        this.devices = {
            mic: [],
            ref: [],
            out: []
        };
        this.prefs = {
            mic: null,
            ref: null,
            out: null,
            nsEnabled: true,
            notchEnabled: true,
            micGain: 1.0,
            trayEnabled: true
        };
        this.state = {};
        this.startTime = Date.now();
        this.statusText = 'Idle';
        this.windowSize = { width: window.innerWidth, height: window.innerHeight };
        this.framesThisSecond = 0;
        this.lastFPSUpdate = 0;
        this.debugMode = false;
        
        // UI layout constants
        this.PANEL_PADDING = 20;
        this.ELEMENT_SPACING = 15;
        this.BUTTON_HEIGHT = 40;
        this.SLIDER_HEIGHT = 20;
        this.CHECKBOX_SIZE = 20;
    }

    init() {
        this.setupCanvas();
        this.setupEventListeners();
        this.loadInitialState();
        this.startRenderLoop();
        console.log('Web ImGui UI initialized successfully');
    }

    setupCanvas() {
        this.canvas = document.createElement('canvas');
        this.canvas.id = 'web-imgui-canvas';
        this.canvas.width = window.innerWidth;
        this.canvas.height = window.innerHeight;
        
        // Apply Dear ImGui window styling
        Object.assign(this.canvas.style, {
            position: 'fixed',
            top: '0',
            left: '0',
            width: '100%',
            height: '100%',
            zIndex: '1000',
            cursor: 'default',
            display: 'block'
        });
        
        document.body.appendChild(this.canvas);
        this.ctx = this.canvas.getContext('2d');
    }

    setupEventListeners() {
        const self = this;
        
        // Mouse events
        this.canvas.addEventListener('mousemove', (e) => {
            this.updateMousePosition(e);
            e.preventDefault();
        });
        
        this.canvas.addEventListener('mousedown', (e) => {
            this.updateMouseButton(e, true);
            e.preventDefault();
        });
        
        this.canvas.addEventListener('mouseup', (e) => {
            this.updateMouseButton(e, false);
            e.preventDefault();
        });
        
        this.canvas.addEventListener('wheel', (e) => {
            this.mouseWheel += e.deltaY * 0.1;
            e.preventDefault();
        });
        
        // Keyboard events
        document.addEventListener('keydown', (e) => {
            this.updateKeyboard(e, true);
            e.preventDefault();
        });
        
        document.addEventListener('keyup', (e) => {
            this.updateKeyboard(e, false);
            e.preventDefault();
        });
        
        // Window events
        window.addEventListener('resize', () => {
            this.onResize();
        });
        
        document.addEventListener('visibilitychange', () => {
            this.onVisibilityChange();
        });
    }

    updateMousePosition(e) {
        const rect = this.canvas.getBoundingClientRect();
        this.mousePos.x = e.clientX - rect.left;
        this.mousePos.y = e.clientY - rect.top;
    }

    updateMouseButton(e, isDown) {
        const button = e.button;
        if (button < this.mouseDown.length) {
            this.mouseDown[button] = isDown;
        }
    }

    updateKeyboard(e, isDown) {
        this.keysDown[e.code] = isDown;
    }

    startRenderLoop() {
        const animate = () => {
            if (document.hidden) return;
            this.update();
            this.render();
            this.animationFrameId = requestAnimationFrame(animate);
        };
        
        this.lastUpdate = Date.now();
        this.framesThisSecond = 0;
        this.lastFPSUpdate = Date.now();
        animate();
    }

    update() {
        const now = Date.now();
        const deltaTime = (now - this.lastUpdate) / 1000.0;
        this.lastUpdate = now;
        
        // Fetch state periodically (every 100ms)
        if (now % 100 < deltaTime * 1000) {
            this.fetchState();
        }
        
        // Update FPS counter
        if (now - this.lastFPSUpdate > 1000) {
            this.fps = this.framesThisSecond;
            this.framesThisSecond = 0;
            this.lastFPSUpdate = now;
            
            if (this.debugMode) {
                console.log(`Web ImGui FPS: ${this.fps}`);
            }
        }
        
        this.framesThisSecond++;
    }

    render() {
        if (!this.ctx) return;
        
        // Clear canvas with Dear ImGui theme
        this.ctx.fillStyle = '#14171c';
        this.ctx.fillRect(0, 0, this.canvas.width, this.canvas.height);
        
        // Draw main Dear ImGui window
        this.drawMainWindow();
    }

    drawMainWindow() {
        const width = this.canvas.width;
        const height = this.canvas.height;
        
        // Main window (full screen like desktop)
        const windowX = 0;
        const windowY = 0;
        const windowWidth = width;
        const windowHeight = height;
        
        // Draw window background
        this.drawWindowBackground(windowX, windowY, windowWidth, windowHeight);
        
        // Draw title bar
        this.drawTitleBar(windowX, windowY, windowWidth);
        
        // Draw content area
        const contentY = 60;
        const contentHeight = windowHeight - contentY - 60;
        
        this.drawContentArea(windowX, contentY, windowWidth, contentHeight);
        
        // Draw status bar
        this.drawStatusBar(windowX, windowHeight - 40, windowWidth);
        
        // Debug info
        if (this.debugMode) {
            this.drawDebugInfo(windowX, windowY);
        }
    }

    drawWindowBackground(x, y, width, height) {
        // Main window background (matching desktop ImGui)
        this.ctx.fillStyle = '#1d2127';
        this.ctx.fillRect(x, y, width, height);
        
        // Window border
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 0.5;
        this.ctx.strokeRect(x, y, width, height);
    }

    drawTitleBar(x, y, width) {
        // Title bar (like desktop ImGui)
        this.ctx.fillStyle = '#1d2127';
        this.ctx.fillRect(x, y, width, 40);
        
        // Status indicator
        const statusX = width - 20;
        this.ctx.fillStyle = this.isRunning ? '#4CAF50' : '#F44336';
        this.ctx.beginPath();
        this.ctx.arc(statusX, y + 20, 8, 0, Math.PI * 2);
        this.ctx.fill();
        
        // Title text
        this.ctx.fillStyle = '#bfd9ff';
        this.ctx.font = 'bold 18px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('AEC Client - Web UI (Dear ImGui)', x + 20, y + 20);
        
        // Version info
        this.ctx.fillStyle = '#8b93a1';
        this.ctx.font = '14px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('v1.10.1 · DTLN-AEC 128 + DTLN-NS · 16 kHz', x + 20, y + 35);
    }

    drawContentArea(x, y, width, height) {
        // Content area split into tabs
        if (this.tab === 'audio') {
            this.drawAudioTab(x, y, width, height);
        } else if (this.tab === 'appearance') {
            this.drawAppearanceTab(x, y, width, height);
        } else if (this.tab === 'about') {
            this.drawAboutTab(x, y, width, height);
        }
    }

    drawAudioTab(x, y, width, height) {
        const panelX = x + 20;
        const panelWidth = width - 40;
        const panelHeight = height - 20;
        
        // Main card
        this.ctx.fillStyle = '#1d2127';
        this.ctx.fillRect(panelX, y, panelWidth, panelHeight);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.strokeRect(panelX, y, panelWidth, panelHeight);
        
        let currentY = y + 20;
        
        // Devices section
        currentY = this.drawSectionHeader(panelX + 20, currentY, 'Devices');
        
        // Rescan button
        this.drawButton(panelX + panelWidth - 100, currentY - 5, 80, 30, 'Rescan', '#2a9d5c');
        currentY += 40;
        
        // Device controls
        currentY += 10;
        
        // Microphone
        currentY = this.drawDeviceControl(panelX + 20, currentY, 'Microphone', this.devices.mic, panelWidth - 100);
        currentY += 80;
        
        // Mic gain control
        currentY = this.drawMicGainControl(panelX + 20, currentY, panelWidth - 60);
        currentY += 50;
        
        // Speaker Reference
        currentY += 20;
        currentY = this.drawDeviceControl(panelX + 20, currentY, 'Speaker Reference (system audio loopback)', this.devices.ref, panelWidth - 60);
        currentY += 80;
        
        // Output
        currentY += 20;
        currentY = this.drawDeviceControl(panelX + 20, currentY, 'Output', this.devices.out, panelWidth - 60);
        currentY += 80;
        
        // Control panel (toggles and start/stop)
        const controlPanelY = y + 40;
        currentY = controlPanelY;
        
        // Noise suppression toggle
        currentY = this.drawToggle(panelX + 20, currentY, 'Noise suppression (DTLN-NS)', this.prefs.nsEnabled);
        currentY += 40;
        
        // Notch toggle
        currentY = this.drawToggle(panelX + 20, currentY, 'Feedback notch (howl suppression)', this.prefs.notchEnabled);
        currentY += 50;
        
        // Start/Stop button
        this.drawActionButton(panelX + 20, currentY, panelWidth - 60, this.isRunning ? 'Stop' : 'Start', this.isRunning ? '#8a2f2f' : '#1f6f43');
        
        // Status area
        currentY = controlPanelY + 120;
        this.drawStatusArea(panelX + 20, currentY, panelWidth - 60);
    }

    drawAppearanceTab(x, y, width, height) {
        const panelX = x + 20;
        const panelWidth = width - 40;
        const panelHeight = height - 20;
        
        this.ctx.fillStyle = '#1d2127';
        this.ctx.fillRect(panelX, y, panelWidth, panelHeight);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.strokeRect(panelX, y, panelWidth, panelHeight);
        
        let currentY = y + 20;
        
        // Title
        this.ctx.fillStyle = '#ffffff';
        this.ctx.font = '18px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('Appearance Settings', panelX + 20, currentY);
        currentY += 40;
        
        // Wallpaper section
        currentY = this.drawSectionHeader(panelX + 20, currentY, 'Wallpaper');
        currentY += 30;
        
        // Wallpaper combo box
        this.drawComboBox(panelX + 20, currentY, 300, '(none)');
        currentY += 50;
        
        // Behavior section
        currentY = this.drawSectionHeader(panelX + 20, currentY, 'Behavior');
        currentY += 30;
        
        // Tray toggle
        currentY = this.drawToggle(panelX + 20, currentY, 'Minimize to system tray (X button hides window)', this.prefs.trayEnabled);
        currentY += 50;
        
        // Help text
        this.ctx.fillStyle = '#8b93a1';
        this.ctx.font = '12px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('Closing the window hides it to the tray instead of quitting — the server keeps running.', panelX + 20, currentY + 10);
    }

    drawAboutTab(x, y, width, height) {
        const panelX = x + 20;
        const panelWidth = width - 40;
        const panelHeight = height - 20;
        
        this.ctx.fillStyle = '#1d2127';
        this.ctx.fillRect(panelX, y, panelWidth, panelHeight);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.strokeRect(panelX, y, panelWidth, panelHeight);
        
        let currentY = y + 20;
        
        // App info
        this.ctx.fillStyle = '#bfd9ff';
        this.ctx.font = 'bold 20px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('AEC Client - Web UI (Dear ImGui)', panelX + 20, currentY);
        currentY += 40;
        
        this.ctx.fillStyle = '#8b93a1';
        this.ctx.font = '14px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('Version v1.10.1', panelX + 20, currentY);
        currentY += 40;
        
        // Description
        this.ctx.fillStyle = '#e8e8e8';
        this.ctx.font = '13px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('A lightweight, open-source acoustic echo cancellation', panelX + 20, currentY);
        currentY += 25;
        this.ctx.fillText('(AEC) client for Windows. Route your microphone through it', panelX + 20, currentY);
        currentY += 25;
        this.ctx.fillText('and pick up a cleaned, echo-free signal in any app.', panelX + 20, currentY);
        currentY += 40;
        
        // Features
        this.ctx.fillStyle = '#ffffff';
        this.ctx.font = '14px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('Features:', panelX + 20, currentY);
        currentY += 30;
        
        const features = [
            'DTLN-AEC 128 echo cancellation + DTLN-NS noise reduction',
            'Adaptive feedback notch + speech gate — howl suppression',
            'Real-time processing with low CPU usage',
            'Works with speakers, earphones, and headsets',
            'Live level meters',
            'Native app window (Edge WebView2) with browser fallback'
        ];
        
        for (let i = 0; i < features.length; i++) {
            this.ctx.fillText('• ' + features[i], panelX + 35, currentY + i * 20);
        }
    }

    drawSectionHeader(x, y, text) {
        this.ctx.fillStyle = '#aab2c0';
        this.ctx.font = 'bold 14px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText(text, x, y);
        return y + 30;
    }

    drawDeviceControl(x, y, label, devices, width) {
        // Label
        this.ctx.fillStyle = '#e8e8e8';
        this.ctx.font = '14px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText(label, x, y);
        
        // Select box
        this.ctx.fillStyle = '#262b33';
        this.ctx.fillRect(x, y + 25, width, 30);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.strokeRect(x, y + 25, width, 30);
        
        // Device name or placeholder
        this.ctx.fillStyle = devices.length > 0 ? '#e8e8e8' : '#8b93a1';
        this.ctx.font = '13px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        const deviceText = devices.length > 0 ? devices[0].name : 'Scanning...';
        this.ctx.fillText(deviceText, x + 10, y + 40);
        
        return y + 65;
    }

    drawMicGainControl(x, y, width) {
        // Label
        this.ctx.fillStyle = '#e8e8e8';
        this.ctx.font = '14px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('Mic gain (preamp) —', x, y);
        this.ctx.textAlign = 'right';
        this.ctx.fillText(`+${this.prefs.micGain > 0 ? '+' : ''}${Math.round((this.prefs.micGain - 1.0) * 100)} dB`, x + width, y);
        
        // Slider track
        const sliderY = y + 25;
        this.ctx.fillStyle = '#262b33';
        this.ctx.fillRect(x, sliderY, width, 8);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.strokeRect(x, sliderY, width, 8);
        
        // Slider fill
        const fillWidth = ((this.prefs.micGain - 0.1) / (4.0 - 0.1)) * width;
        this.ctx.fillStyle = '#2a9d5c';
        this.ctx.fillRect(x, sliderY, fillWidth, 8);
        
        // Slider thumb
        this.ctx.fillStyle = '#4CAF50';
        this.ctx.beginPath();
        this.ctx.arc(x + fillWidth, sliderY + 4, 6, 0, Math.PI * 2);
        this.ctx.fill();
        
        // Hint text
        this.ctx.fillStyle = '#8b93a1';
        this.ctx.font = '11px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        const hintY = sliderY + 20;
        this.ctx.fillText('Amplifies quiet microphones before processing', x, hintY);
        this.ctx.fillText('(-12…+12 dB, live). Boosting also amplifies background noise —', x, hintY + 15);
        this.ctx.fillText('watch the Mic in meter for clipping.', x, hintY + 30);
        
        return hintY + 45;
    }

    drawToggle(x, y, label, isChecked) {
        this.ctx.fillStyle = '#e8e8e8';
        this.ctx.font = '14px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText(label, x, y);
        
        // Checkbox
        const checkboxX = x;
        const checkboxY = y - 5;
        this.ctx.fillStyle = isChecked ? '#4CAF50' : '#3a4048';
        this.ctx.fillRect(checkboxX, checkboxY, 20, 20);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.strokeRect(checkboxX, checkboxY, 20, 20);
        
        if (isChecked) {
            this.ctx.fillStyle = '#ffffff';
            this.ctx.font = '16px system-ui, sans-serif';
            this.ctx.textAlign = 'center';
            this.ctx.textBaseline = 'middle';
            this.ctx.fillText('✓', checkboxX + 10, checkboxY + 10);
        }
        
        return y + 40;
    }

    drawActionButton(x, y, width, text, color) {
        this.ctx.fillStyle = color;
        this.ctx.fillRect(x, y, width, 40);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.strokeRect(x, y, width, 40);
        
        this.ctx.fillStyle = '#ffffff';
        this.ctx.font = 'bold 16px system-ui, sans-serif';
        this.ctx.textAlign = 'center';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText(text, x + width / 2, y + 20);
    }

    drawComboBox(x, y, width, placeholder) {
        this.ctx.fillStyle = '#262b33';
        this.ctx.fillRect(x, y, width, 30);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.strokeRect(x, y, width, 30);
        
        this.ctx.fillStyle = '#e8e8e8';
        this.ctx.font = '13px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText(placeholder, x + 10, y + 15);
        
        // Dropdown arrow
        this.ctx.fillStyle = '#8b93a1';
        this.ctx.beginPath();
        this.ctx.moveTo(x + width - 20, y + 10);
        this.ctx.lineTo(x + width - 10, y + 10);
        this.ctx.lineTo(x + width - 15, y + 20);
        this.ctx.closePath();
        this.ctx.fill();
    }

    drawStatusArea(x, y, width) {
        this.ctx.fillStyle = '#9fd3a8';
        this.ctx.font = '14px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText(this.statusText, x, y);
        
        // Live meters
        y += 30;
        this.ctx.fillStyle = '#aab2c0';
        this.ctx.font = '12px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText('Live Levels:', x, y);
        
        // Draw simple meters
        const meterY = y + 20;
        this.drawSimpleMeter(x, meterY, 280, this.state.inRms || 0, 3000, 'Mic');
        this.drawSimpleMeter(x + 300, meterY, 280, this.state.outRms || 0, 3000, 'Out');
    }

    drawSimpleMeter(x, y, width, value, maxValue, label) {
        const height = 20;
        
        this.ctx.fillStyle = '#aab2c0';
        this.ctx.font = '12px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText(label, x, y);
        
        // Meter background
        this.ctx.fillStyle = '#262b33';
        this.ctx.fillRect(x, y + 15, width, height);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.strokeRect(x, y + 15, width, height);
        
        // Meter fill
        const fillWidth = Math.min((value / maxValue) * width, width);
        const meterColor = value > maxValue * 0.8 ? '#F44336' : value > maxValue * 0.6 ? '#FF9800' : '#4CAF50';
        this.ctx.fillStyle = meterColor;
        this.ctx.fillRect(x, y + 15, fillWidth, height);
        
        // Peak marker
        const peakWidth = 2;
        this.ctx.fillStyle = 'rgba(255, 255, 255, 0.5)';
        this.ctx.fillRect(x + fillWidth, y + 15, peakWidth, height);
    }

    drawStatusBar(x, y, width) {
        // Status bar (like desktop)
        this.ctx.fillStyle = '#1d2127';
        this.ctx.fillRect(x, y, width, 40);
        this.ctx.strokeStyle = '#3a4048';
        this.ctx.lineWidth = 1;
        this.ctx.beginPath();
        this.ctx.moveTo(x, y);
        this.ctx.lineTo(x + width, y);
        this.ctx.stroke();
        
        // Status text
        this.ctx.fillStyle = '#8b93a1';
        this.ctx.font = '12px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'middle';
        this.ctx.fillText(`AEC Web UI (Dear ImGui) v1.10.1 · FPS: ${this.fps}`, x + 10, y + 20);
    }

    drawDebugInfo(x, y) {
        this.ctx.fillStyle = '#ff9d9d';
        this.ctx.font = '10px system-ui, sans-serif';
        this.ctx.textAlign = 'left';
        this.ctx.textBaseline = 'top';
        this.ctx.fillText(`Mouse: (${Math.round(this.mousePos.x)}, ${Math.round(this.mousePos.y)})`, x + 10, y + 10);
        this.ctx.fillText(`Keys: ${Object.keys(this.keysDown).join(', ')}`, x + 10, y + 25);
        this.ctx.fillText(`Tab: ${this.tab}`, x + 10, y + 40);
        this.ctx.fillText(`Running: ${this.isRunning}`, x + 10, y + 55);
    }

    fetchState() {
        fetch('/api/state')
            .then(response => response.json())
            .then(data => {
                this.state = data;
                this.isRunning = data.running;
                this.updateStatus();
            })
            .catch(error => {
                console.log('Failed to fetch state:', error);
            });
        
        fetch('/api/prefs')
            .then(response => response.json())
            .then(data => {
                this.prefs = data;
            })
            .catch(error => {
                console.log('Failed to fetch prefs:', error);
            });
        
        fetch('/api/devices')
            .then(response => response.json())
            .then(data => {
                this.devices = data;
            })
            .catch(error => {
                console.log('Failed to fetch devices:', error);
            });
    }

    updateStatus() {
        if (this.isRunning) {
            const elapsed = Date.now() - this.startTime;
            const minutes = Math.floor(elapsed / 60000);
            const seconds = Math.floor((elapsed % 60000) / 1000);
            this.statusText = `Running  ${minutes}:${seconds.toString().padStart(2, '0')}`;
        } else {
            this.statusText = 'Idle';
        }
    }

    loadInitialState() {
        this.fetchState();
    }

    onResize() {
        this.windowSize.width = window.innerWidth;
        this.windowSize.height = window.innerHeight;
        this.render();
    }

    onVisibilityChange() {
        if (document.hidden) {
            cancelAnimationFrame(this.animationFrameId);
        } else {
            this.startRenderLoop();
        }
    }

    // Public API
    setTab(tab) {
        this.tab = tab;
        this.render();
    }

    setRunning(running) {
        this.isRunning = running;
        if (running) {
            this.startTime = Date.now();
        }
        this.updateStatus();
        this.render();
    }

    setMicGain(value) {
        this.prefs.micGain = value;
        this.render();
    }

    setNSToggled(enabled) {
        this.prefs.nsEnabled = enabled;
        this.render();
    }

    setNotchToggled(enabled) {
        this.prefs.notchEnabled = enabled;
        this.render();
    }

    setTrayEnabled(enabled) {
        this.prefs.trayEnabled = enabled;
        this.render();
    }

    toggleDebug() {
        this.debugMode = !this.debugMode;
        console.log(`Debug mode: ${this.debugMode ? 'ON' : 'OFF'}`);
    }
}

// Global initialization
let webImGuiUI = null;

function initWebImGui() {
    // Create Web ImGui instance
    webImGuiUI = new WebImGuiUI();
    webImGuiUI.init();
    console.log('Web ImGui UI initialized successfully');
}

// Expose global API
window.WebImGui = {
    init: initWebImGui,
    setTab: (tab) => webImGuiUI && webImGuiUI.setTab(tab),
    setRunning: (running) => webImGuiUI && webImGuiUI.setRunning(running),
    setMicGain: (value) => webImGuiUI && webImGuiUI.setMicGain(value),
    setNSToggled: (enabled) => webImGuiUI && webImGuiUI.setNSToggled(enabled),
    setNotchToggled: (enabled) => webImGuiUI && webImGuiUI.setNotchToggled(enabled),
    setTrayEnabled: (enabled) => webImGuiUI && webImGuiUI.setTrayEnabled(enabled),
    toggleDebug: () => webImGuiUI && webImGuiUI.toggleDebug()
};

// Initialize on page load
window.addEventListener('load', () => {
    // Always initialize - this is a pure JavaScript implementation
    // that doesn't require WebAssembly or external modules
    initWebImGui();
});
// Timeout handler for UI initialization
window.addEventListener('load', function() {
    setTimeout(function() {
        if (!document.getElementById('root') || document.getElementById('root').innerHTML.includes('Loading AEC Web UI')) {
            console.log('UI initialization timeout - showing fallback');
            document.getElementById('root').innerHTML = 
                '<div style="text-align: center; padding: 50px; color: #e8e8e8; font-family: system-ui, sans-serif;">' +
                '<h1>AEC Client - Web UI (Dear ImGui)</h1>' +
                '<p>UI initialization timed out. Please refresh the page.</p>' +
                '<button onclick="location.reload()" style="padding: 10px 20px; margin: 10px; background: #2a9d5c; color: white; border: none; border-radius: 4px; cursor: pointer;">Refresh Page</button>' +
                '</div>';
        }
    }, 10000); // 10 second timeout
});
