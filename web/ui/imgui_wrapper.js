// WebAssembly Dear ImGui UI Wrapper
// This module provides the bridge between Python backend and WebAssembly Dear ImGui

// Global state
let gWasmModule = null;
let gImGuiContext = null;
let gIsVisible = true;
let gLastTime = 0;
let gFPS = 0;

// Import WebAssembly module (simulated for demonstration)
function importWasmModule() {
    return new Promise((resolve) => {
        // In production, this would load the actual WebAssembly module
        // For demonstration, we'll simulate it
        console.log("Loading Dear ImGui WebAssembly module...");
        
        setTimeout(() => {
            // Simulated WebAssembly module with Dear ImGui bindings
            const wasmModule = {
                // Core Dear ImGui functions
                ImGui_NewFrame: () => {
                    if (!gImGuiContext) {
                        // Initialize ImGui context
                        gImGuiContext = {};
                    }
                    return true;
                },
                ImGui_Render: () => {
                    // Render to canvas
                    renderToCanvas();
                },
                ImGui_GetIO: () => {
                    return {
                        DisplaySize: { x: window.innerWidth, y: window.innerHeight },
                        DeltaTime: 1.0 / 60.0,
                        MouseDown: [false, false, false, false],
                        MousePos: { x: 0, y: 0 },
                        MouseWheel: 0,
                        KeysDown: {},
                        WantCaptureMouse: false,
                        WantCaptureKeyboard: false,
                        FontGlobalScale: 1.0
                    };
                },
                // Helper functions
                GetTime: () => performance.now() / 1000.0,
                GetDeltaTime: () => {
                    const now = performance.now() / 1000.0;
                    const delta = now - gLastTime;
                    gLastTime = now;
                    return delta > 0 ? delta : 1.0 / 60.0;
                },
                CalcDeltaTime: () => {
                    const now = performance.now() / 1000.0;
                    const delta = now - gLastTime;
                    gLastTime = now;
                    if (delta >= 0 && delta < 1.0) {
                        return delta;
                    }
                    return 1.0 / 60.0;
                }
            };
            
            resolve(wasmModule);
        }, 100);
    });
}

// Initialize Dear ImGui
function IMGUI_Init(wasmModule) {
    gWasmModule = wasmModule;
    gLastTime = performance.now() / 1000.0;
    
    // Setup canvas for rendering
    setupCanvas();
    
    // Setup event listeners
    setupEventListeners();
    
    console.log("Dear ImGui Web UI initialized");
}

function setupCanvas() {
    const canvas = document.createElement('canvas');
    canvas.id = 'imgui-canvas';
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;
    canvas.style.position = 'fixed';
    canvas.style.top = '0';
    canvas.style.left = '0';
    canvas.style.width = '100%';
    canvas.style.height = '100%';
    canvas.style.zIndex = '1000';
    canvas.style.pointerEvents = 'auto';
    canvas.style.display = 'block';
    
    document.body.appendChild(canvas);
    
    const ctx = canvas.getContext('2d');
    if (!ctx) {
        console.error("Failed to get 2D context for canvas");
        return;
    }
    
    // Store context for rendering
    window.imguiCanvas = canvas;
    window.imguiCtx = ctx;
}

function setupEventListeners() {
    const canvas = window.imguiCanvas;
    const io = gWasmModule ? gWasmModule.GetIO() : {};
    
    // Mouse events
    canvas.addEventListener('mousemove', (e) => {
        if (io && io.WantCaptureMouse) {
            e.preventDefault();
        }
        updateMousePosition(e);
    });
    
    canvas.addEventListener('mousedown', (e) => {
        if (io && io.WantCaptureMouse) {
            e.preventDefault();
        }
        updateMouseButton(e, true);
    });
    
    canvas.addEventListener('mouseup', (e) => {
        if (io && io.WantCaptureMouse) {
            e.preventDefault();
        }
        updateMouseButton(e, false);
    });
    
    canvas.addEventListener('wheel', (e) => {
        if (io && io.WantCaptureMouse) {
            e.preventDefault();
        }
        updateMouseWheel(e);
    });
    
    // Keyboard events
    document.addEventListener('keydown', (e) => {
        if (io && io.WantCaptureKeyboard) {
            e.preventDefault();
        }
        updateKeyboard(e, true);
    });
    
    document.addEventListener('keyup', (e) => {
        if (io && io.WantCaptureKeyboard) {
            e.preventDefault();
        }
        updateKeyboard(e, false);
    });
    
    // Window events
    window.addEventListener('resize', () => {
        if (io) {
            io.DisplaySize.x = window.innerWidth;
            io.DisplaySize.y = window.innerHeight;
        }
    });
}

function updateMousePosition(e) {
    const canvas = window.imguiCanvas;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    
    if (gWasmModule) {
        gWasmModule.GetIO().MousePos.x = x;
        gWasmModule.GetIO().MousePos.y = y;
    }
}

function updateMouseButton(e, isDown) {
    if (!gWasmModule) return;
    
    const io = gWasmModule.GetIO();
    const button = getMouseButtonFromEvent(e);
    
    if (button >= 0 && button < io.MouseDown.length) {
        io.MouseDown[button] = isDown;
    }
}

function updateMouseWheel(e) {
    if (!gWasmModule) return;
    
    const io = gWasmModule.GetIO();
    io.MouseWheel += e.deltaY * 0.1;
}

function updateKeyboard(e, isDown) {
    if (!gWasmModule) return;
    
    const io = gWasmModule.GetIO();
    const key = getKeyFromEvent(e);
    
    if (key) {
        io.KeysDown[key] = isDown;
    }
}

function getMouseButtonFromEvent(e) {
    switch (e.button) {
        case 0: return 0; // Left
        case 1: return 1; // Right
        case 2: return 2; // Middle
        default: return -1;
    }
}

function getKeyFromEvent(e) {
    // Map keyboard events to ImGui key indices
    const keyMap = {
        'KeyA': 0, 'KeyB': 1, 'KeyC': 2, 'KeyD': 3, 'KeyE': 4,
        'KeyF': 5, 'KeyG': 6, 'KeyH': 7, 'KeyI': 8, 'KeyJ': 9,
        'KeyK': 10, 'KeyL': 11, 'KeyM': 12, 'KeyN': 13, 'KeyO': 14,
        'KeyP': 15, 'KeyQ': 16, 'KeyR': 17, 'KeyS': 18, 'KeyT': 19,
        'KeyU': 20, 'KeyV': 21, 'KeyW': 22, 'KeyX': 23, 'KeyY': 24,
        'KeyZ': 25, 'Digit0': 26, 'Digit1': 27, 'Digit2': 28,
        'Digit3': 29, 'Digit4': 30, 'Digit5': 31, 'Digit6': 32,
        'Digit7': 33, 'Digit8': 34, 'Digit9': 35, 'Escape': 36,
        'Enter': 37, 'Space': 38, 'Tab': 39, 'Backspace': 40,
        'ArrowLeft': 41, 'ArrowRight': 42, 'ArrowUp': 43, 'ArrowDown': 44
    };
    
    return keyMap[e.code] || -1;
}

function renderToCanvas() {
    if (!window.imguiCanvas || !window.imguiCtx) return;
    
    const canvas = window.imguiCanvas;
    const ctx = window.imguiCtx;
    
    // Clear canvas
    ctx.fillStyle = '#14171c';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    
    // Render UI (simplified - in real implementation would render actual ImGui draw data)
    renderUIElements(ctx);
    
    // Update FPS counter
    updateFPSCounter();
}

function renderUIElements(ctx) {
    // This is a simplified renderer for demonstration
    // In production, you would render actual ImGui draw lists
    
    // Draw a simple test UI
    ctx.fillStyle = '#1d2127';
    ctx.fillRect(50, 50, 400, 300);
    
    ctx.fillStyle = '#bfd9ff';
    ctx.font = '24px system-ui, sans-serif';
    ctx.fillText('AEC Client - Web UI (Dear ImGui)', 80, 100);
    
    ctx.fillStyle = '#e8e8e8';
    ctx.font = '16px system-ui, sans-serif';
    ctx.fillText('Status: Running', 80, 150);
    ctx.fillText('Engine: DTLN-AEC 128', 80, 180);
    ctx.fillText('Sample Rate: 16000 Hz', 80, 210);
    ctx.fillText('MemGain: +0.0 dB', 80, 240);
    
    // Draw some basic controls
    drawButton(ctx, 80, 280, 200, 40, 'Start', '#1f6f43');
    drawButton(ctx, 290, 280, 150, 40, 'Stop', '#8a2f2f');
}

function drawButton(ctx, x, y, width, height, text, color) {
    // Draw button background
    ctx.fillStyle = color;
    ctx.fillRect(x, y, width, height);
    
    // Draw button text
    ctx.fillStyle = '#ffffff';
    ctx.font = '14px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(text, x + width / 2, y + height / 2);
}

function updateFPSCounter() {
    const now = performance.now();
    const delta = now - (gLastTime * 1000);
    
    if (delta >= 1000) {
        gFPS = Math.round(1000 / delta);
        gLastTime = now / 1000.0;
        
        // Display FPS in console
        if (gFPS % 60 === 0) {
            console.log(`Dear ImGui FPS: ${gFPS}`);
        }
    }
}

function setupUI(wasmModule) {
    // Setup main UI loop
    function gameLoop() {
        if (!gIsVisible) {
            requestAnimationFrame(gameLoop);
            return;
        }
        
        // Update ImGui frame
        if (wasmModule && wasmModule.ImGui_NewFrame) {
            wasmModule.ImGui_NewFrame();
            
            // Render UI
            if (wasmModule.ImGui_Render) {
                wasmModule.ImGui_Render();
            }
        }
        
        // Schedule next frame
        requestAnimationFrame(gameLoop);
    }
    
    // Start the game loop
    gameLoop();
}

function IMGUI_Resize(width, height) {
    if (window.imguiCanvas) {
        window.imguiCanvas.width = width;
        window.imguiCanvas.height = height;
        
        if (gWasmModule && gWasmModule.GetIO) {
            gWasmModule.GetIO().DisplaySize.x = width;
            gWasmModule.GetIO().DisplaySize.y = height;
        }
    }
}

function IMGUI_SetVisibility(visible) {
    gIsVisible = visible;
    if (!visible) {
        cancelAnimationFrame(window.imguiFrameId);
    } else {
        gameLoop();
    }
}

// Expose global functions for backward compatibility
window.IMGUI_Init = IMGUI_Init;
window.IMGUI_Resize = IMGUI_Resize;
window.IMGUI_SetVisibility = IMGUI_SetVisibility;
