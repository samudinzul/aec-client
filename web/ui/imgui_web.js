// Dear ImGui-style Web UI — DOM implementation, no WebAssembly.
//
// Mirrors the desktop client (src/main.cpp DrawUI):
// app header with status dot + Running/Idle, a horizontal
// Audio / Appearance / About tab bar, and the same sections,
// labels and footer behavior. Controls the web backend, whose
// engine is fixed to DTLN-AEC 128 @ 16 kHz, so engine-specific
// widgets the server has no endpoint for (profile switching,
// WPE, sample rate, listen-to-myself) are shown the way the
// desktop shows a forced choice: fixed/disabled with a note.

'use strict';

let webImGuiUI = null;

const APP_VERSION = 'v1.11.0';

class WebImGuiUI {
    constructor() {
        this.running = false;
        this.startStamp = 0;
        this.ws = null;
        this.wsRetry = 0;
        this.inited = false;
        this.el = {};
        this.lastDevices = null;
        this.peak = { in: 0, out: 0 };
    }

    init() {
        if (this.inited) return;
        this.inited = true;
        const root = document.getElementById('root');
        if (root) root.style.display = 'none';
        this.build();
        this.bind();
        this.applyWallpaper(false);
        this.refreshToggles();
        this.connect();
        this.loadDevices();
        console.log('Web ImGui UI initialized successfully');
    }

    // ---- DOM construction (mirrors DrawUI / Draw*Tab) ----------------
    mk(tag, cls, html) {
        const e = document.createElement(tag);
        if (cls) e.className = cls;
        if (html !== undefined) e.innerHTML = html;
        return e;
    }

    sep(text) {
        const d = this.mk('div', 'sep', text || '');
        return d;
    }

    build() {
        const app = this.mk('div');
        app.id = 'imgui-app';
        app.style.cssText = 'position:fixed;inset:0;overflow-y:auto;padding:20px 20px 40px;box-sizing:border-box;';

        // Header: APP_NAME (accent) + version, status dot + Running/Idle right.
        const header = this.mk('div', 'flex flex-between');
        header.style.cssText = 'max-width:640px;margin:0 auto 4px;';
        header.innerHTML =
            '<div><span class="text-accent" style="font-weight:bold;font-size:15px">AEC Client</span> ' +
            '<span class="text-secondary" style="font-size:13px">' + APP_VERSION +
            ' · DTLN-AEC 128 + DTLN-NS · 16 kHz</span></div>' +
            '<div style="display:flex;align-items:center;gap:8px">' +
            '<span id="ig-dot" class="status-dot inactive"></span>' +
            '<span id="ig-runstate" class="text-secondary" style="font-size:13px">Idle</span></div>';
        app.appendChild(header);
        this.el.dot = header.querySelector('#ig-dot');
        this.el.runstate = header.querySelector('#ig-runstate');

        // Tab bar: always horizontal, like ImGui::BeginTabBar.
        const tabs = this.mk('div', 'tabs');
        tabs.style.cssText = 'max-width:640px;margin:0 auto 6px;';
        this.el.tabBtns = {};
        for (const name of ['audio', 'appearance', 'about']) {
            const b = this.mk('button', 'tab' + (name === 'audio' ? ' active' : ''),
                name[0].toUpperCase() + name.slice(1));
            b.type = 'button';
            tabs.appendChild(b);
            this.el.tabBtns[name] = b;
        }
        app.appendChild(tabs);

        this.el.panels = {};
        this.el.panels.audio = this.buildAudioPanel();
        this.el.panels.appearance = this.buildAppearancePanel();
        this.el.panels.about = this.buildAboutPanel();
        for (const name of ['audio', 'appearance', 'about']) {
            if (name !== 'audio') this.el.panels[name].style.display = 'none';
            app.appendChild(this.el.panels[name]);
        }

        document.body.appendChild(app);
        this.el.app = app;
    }

    buildAudioPanel() {
        const p = this.mk('div');
        p.style.cssText = 'max-width:640px;margin:0 auto;';

        // --- Profile (fixed engine on the web backend) ---
        p.appendChild(this.sep('Profile'));
        const prof = this.mk('div', 'frow');
        prof.innerHTML = '<span class="flabel">Profile</span>' +
            '<select id="ig-profile" disabled><option>DTLN-AEC 128</option></select>';
        p.appendChild(prof);
        const stages = this.mk('div');
        stages.innerHTML =
            '<label class="chkline"><input type="checkbox" id="ig-nsOn" checked>Noise suppression (DTLN-NS)</label>';
        p.appendChild(stages);

        // --- Devices (Refresh top-right, dot + 190px label rows) ---
        const devHead = this.sep('Devices');
        devHead.innerHTML += '<button id="ig-rescan" type="button" class="button button-small button-secondary" style="margin-left:auto">Refresh</button>';
        p.appendChild(devHead);
        const rows = this.mk('div');
        rows.innerHTML =
            '<div class="frow"><span class="flabel"><span id="ig-dotMic" class="status-dot inactive"></span>Your microphone</span><select id="ig-mic"></select></div>' +
            '<div class="frow"><span class="flabel"><span id="ig-dotRef" class="status-dot inactive"></span>Your speakers</span><select id="ig-ref"></select></div>' +
            '<div class="frow" id="ig-rowOut"><span class="flabel"><span id="ig-dotOut" class="status-dot inactive"></span>Send cleaned sound to</span><select id="ig-out"></select></div>' +
            '<div id="ig-cablewarn" class="warn" hidden>VB-CABLE not found - install VB-CABLE and enable CABLE Input, then hit Refresh.</div>' +
            '<div id="ig-devhint" class="hint" style="color:#ffd75e"></div>' +
            '<label class="chkline"><input type="checkbox" id="ig-listen">Listen to myself</label>' +
            '<div class="hint">Hear yourself through your speakers instead of sending to voice apps — for testing. ' +
            'Applies after Stop -&gt; Start; the Output row above is hidden while on.</div>';
        p.appendChild(rows);

        // --- More (Processing + Levels, like the desktop CollapsingHeader) ---
        const more = document.createElement('details');
        more.className = 'ig-more';
        more.innerHTML =
            '<summary>More</summary>' +
            '<div class="sep">Processing</div>' +
            '<div class="frow"><span class="flabel">Sample Rate</span><span class="text-secondary" style="font-size:14px">16 kHz (automatic)</span></div>' +
            '<div class="sep">Levels</div>' +
            '<div class="frow"><span class="flabel">Microphone level</span>' +
            '<div class="fgrow" style="display:flex;gap:8px;align-items:center">' +
            '<input type="range" id="ig-micGain" min="0" max="200" step="1" value="100" style="flex:1">' +
            '<span id="ig-gainVal" class="text-secondary" style="min-width:48px;text-align:right;font-size:13px">100%</span>' +
            '</div></div>';
        p.appendChild(more);

        // --- Footer (Audio tab only): Start / Reset + status; Live Levels while running ---
        p.appendChild(this.sep(''));
        const foot = this.mk('div', 'footer-btns');
        foot.innerHTML =
            '<button id="ig-startBtn" type="button" class="button">Start</button>' +
            '<button id="ig-resetBtn" type="button" class="button button-secondary">Reset to defaults</button>' +
            '<span id="ig-status" class="footer-status">Stopped.</span>';
        p.appendChild(foot);

        const levels = this.mk('div');
        levels.id = 'ig-levels';
        levels.hidden = true;
        levels.innerHTML =
            '<div class="sep">Live Levels</div>' +
            '<div class="frow"><span class="flabel">Mic</span><canvas id="ig-mIn" class="meter" width="420" height="26"></canvas></div>' +
            '<div class="frow"><span class="flabel">Out</span><canvas id="ig-mOut" class="meter" width="420" height="26"></canvas></div>';
        p.appendChild(levels);
        this.el.levels = levels;

        const err = this.mk('div', 'hint');
        err.id = 'ig-err';
        err.style.cssText = 'color:#ff9d9d;white-space:pre-wrap';
        p.appendChild(err);
        return p;
    }

    buildAppearancePanel() {
        const p = this.mk('div');
        p.style.cssText = 'max-width:640px;margin:0 auto;';
        p.appendChild(this.sep('Wallpaper'));
        const w = this.mk('div', 'frow');
        w.innerHTML = '<span class="flabel">Wallpaper</span>' +
            '<div class="fgrow" style="display:flex;gap:8px">' +
            '<select id="ig-wallpaper" style="flex:1">' +
            '<option value="none">Dark (default)</option>' +
            '<option value="slate">Slate</option>' +
            '<option value="abyss">Abyss teal</option>' +
            '<option value="ember">Ember</option>' +
            '</select>' +
            '<button id="ig-wpReload" type="button" class="button button-small button-secondary">Reload</button>' +
            '</div>';
        p.appendChild(w);
        const wh = this.mk('div', 'hint', 'A browser-side backdrop (the native app draws real images from wallpapers/).');
        p.appendChild(wh);
        p.appendChild(this.sep('Behavior'));
        const b = this.mk('div');
        b.innerHTML =
            '<label class="chkline"><input type="checkbox" id="ig-trayOn">Minimize to system tray (X button hides window)</label>' +
            '<div class="hint">When ON: X hides the window, tray icon stays active. When OFF: X closes the app completely. ' +
            '(Needs the native window; hidden in a browser tab.)</div>' +
            '<label class="chkline"><input type="checkbox" disabled>Show experimental engines (WebRTC AEC3, NKF-AEC)</label>' +
            '<div class="hint">The web backend runs DTLN-AEC 128 only; extra engines need the native app.</div>';
        p.appendChild(b);
        return p;
    }

    buildAboutPanel() {
        const p = this.mk('div');
        p.style.cssText = 'max-width:640px;margin:0 auto;';
        p.appendChild(this.sep('About'));
        const a = this.mk('div');
        a.innerHTML =
            '<p class="text-accent" style="font-size:15px;font-weight:bold;margin:4px 0 0">AEC Client</p>' +
            '<p class="text-secondary" style="font-size:13px">Version ' + APP_VERSION + '</p>' +
            '<p style="font-size:14px;line-height:1.45">A lightweight, open-source acoustic echo cancellation ' +
            '(AEC) client for Windows. Route your microphone through it and pick up a cleaned, echo-free ' +
            'signal in any app (Discord, Zoom, Teams, etc.).</p>';
        p.appendChild(a);
        p.appendChild(this.sep('Features'));
        const f = this.mk('div');
        f.innerHTML = '<ul style="font-size:13px;padding-left:20px;margin:4px 0">' +
            '<li>DTLN-AEC 128 echo cancellation + DTLN-NS noise reduction (16 kHz)</li>' +
            '<li>Real-time processing with low CPU usage</li>' +
            '<li>Works with speakers, earphones, and headsets</li>' +
            '<li>Live level meters with peak hold</li>' +
            '<li>Native app window (Edge WebView2) with browser fallback</li>' +
            '</ul>';
        p.appendChild(f);
        p.appendChild(this.sep('Credits'));
        const c = this.mk('div');
        c.innerHTML = '<ul style="font-size:13px;padding-left:20px;margin:4px 0">' +
            '<li>DTLN-AEC — Westhausen &amp; Meyer (ICASSP 2021, MIT)</li>' +
            '<li>DTLN-NS — networkedaudio port of breizhn/DTLN denoise (MIT)</li>' +
            '<li>Dear ImGui — Omar Cornut (MIT) — layout reference for this page</li>' +
            '<li>FastAPI, uvicorn, pywebview, pystray, numpy, ai-edge-litert</li>' +
            '</ul><div class="hint">Made with Python · source: github.com/samudinzul/aec-client</div>';
        p.appendChild(c);
        return p;
    }

    // ---- Wiring ------------------------------------------------------
    $(id) { return this.el.app.querySelector('#' + id); }

    bind() {
        const q = (id) => this.$(id);
        this.el.mic = q('ig-mic'); this.el.ref = q('ig-ref'); this.el.out = q('ig-out');
        this.el.gain = q('ig-micGain'); this.el.gainVal = q('ig-gainVal');
        this.el.ns = q('ig-nsOn');
        this.el.listen = q('ig-listen');
        this.el.rowOut = q('ig-rowOut');
        this.el.tray = q('ig-trayOn');
        this.el.start = q('ig-startBtn'); this.el.reset = q('ig-resetBtn');
        this.el.status = q('ig-status');
        this.el.err = q('ig-err'); this.el.devhint = q('ig-devhint');
        this.el.mIn = q('ig-mIn'); this.el.mOut = q('ig-mOut');
        this.el.rescan = q('ig-rescan');
        this.el.cablewarn = q('ig-cablewarn');
        this.el.dotMic = q('ig-dotMic'); this.el.dotRef = q('ig-dotRef'); this.el.dotOut = q('ig-dotOut');
        this.el.wp = q('ig-wallpaper');

        for (const name of ['audio', 'appearance', 'about']) {
            this.el.tabBtns[name].addEventListener('click', () => this.setTab(name));
        }
        // Tray is a native-window feature: hide the Appearance tab's
        // relevance note only — the tab itself stays (desktop parity).
        if (typeof window.pywebview === 'undefined') {
            window.addEventListener('pywebviewready', () => {}, { once: true });
        }

        [this.el.mic, this.el.ref, this.el.out].forEach((s) =>
            s.addEventListener('change', () => {
                this.savePrefs();
                this.el.devhint.textContent = this.running ? 'Stop, then Start to apply the new device.' : '';
            }));
        this.el.rescan.addEventListener('click', () => this.loadDevices());
        this.el.ns.addEventListener('change', () => this.savePrefs());
        this.el.listen.addEventListener('change', () => {
            this.savePrefs();
            this.showListen();
            this.el.devhint.textContent = this.running ? 'Stop, then Start to apply the new output.' : '';
        });
        this.el.gain.addEventListener('input', () => { this.showGain(); this.savePrefs(); });
        this.el.tray.addEventListener('change', () => this.savePrefs());
        this.el.start.addEventListener('click', () => this.toggleStart());
        this.el.reset.addEventListener('click', () => this.resetDefaults());
        this.el.wp.addEventListener('change', () => this.applyWallpaper(true));
        this.$('ig-wpReload').addEventListener('click', () => this.applyWallpaper(true));
    }

    api(method, path, body) {
        return fetch(path, {
            method,
            headers: body ? { 'Content-Type': 'application/json' } : undefined,
            body: body ? JSON.stringify(body) : undefined,
        }).then((r) => r.json());
    }

    prefVal(sel) { const v = sel.value; return v === '' ? null : +v; }

    setTab(name) {
        for (const k of ['audio', 'appearance', 'about']) {
            this.el.tabBtns[k].classList.toggle('active', k === name);
            this.el.panels[k].style.display = k === name ? '' : 'none';
        }
    }

    showGain() {
        this.el.gainVal.textContent = this.el.gain.value + '%';
    }

    showListen() {
        // Desktop parity: the Output row is hidden while
        // Listen-to-myself reroutes to Your speakers.
        this.el.rowOut.style.display = this.el.listen.checked ? 'none' : '';
    }

    savePrefs() {
        this.api('POST', '/api/prefs', {
            mic: this.prefVal(this.el.mic), ref: this.prefVal(this.el.ref),
            out: this.prefVal(this.el.out),
            nsEnabled: this.el.ns.checked,
            micGain: Math.min(4.0, Math.max(0.1, (+this.el.gain.value) / 100)),
            trayEnabled: this.el.tray.checked,
            listenToSelf: this.el.listen.checked,
        }).catch(() => {});
    }

    refreshToggles() {
        this.api('GET', '/api/state').then((st) => {
            if (st && st.nsEnabled !== undefined) this.el.ns.checked = !!+st.nsEnabled;
            if (st && st.micGain !== undefined)
                this.el.gain.value = Math.min(200, Math.max(0, Math.round(+st.micGain * 100)));
            this.showGain();
        }).catch(() => this.showGain());
        this.api('GET', '/api/prefs').then((p) => {
            if (p && 'trayEnabled' in p) this.el.tray.checked = !!p.trayEnabled;
            if (p && 'listenToSelf' in p) this.el.listen.checked = !!p.listenToSelf;
            this.showListen();
        }).catch(() => {});
    }

    setDot(el, on) { el.className = 'status-dot ' + (on ? 'active' : 'inactive'); }

    async loadDevices() {
        for (const s of [this.el.mic, this.el.ref, this.el.out]) {
            s.innerHTML = '';
            const o = document.createElement('option');
            o.textContent = 'Scanning…';
            s.appendChild(o);
        }
        try {
            const d = await this.api('GET', '/api/devices');
            if (!d || !Array.isArray(d.capture)) throw new Error('bad device payload');
            this.lastDevices = d;
            if (d.errors && (d.errors.audio || d.errors.loopback))
                this.el.err.textContent = ['devices:', d.errors.audio || '', d.errors.loopback || ''].join(' ').trim();
            const prevText = (s) => (s.selectedIndex >= 0 && s.options[s.selectedIndex] ? s.options[s.selectedIndex].text : '');
            const pm = prevText(this.el.mic), pr = prevText(this.el.ref), po = prevText(this.el.out);
            const fill = (sel, list, defIdx, prev) => {
                sel.innerHTML = '';
                (list || []).forEach((x) => {
                    const o = document.createElement('option');
                    o.value = x.index; o.textContent = x.name; sel.appendChild(o);
                });
                let at = (list || []).findIndex((x) => x.name === prev);
                if (at < 0) at = (list || []).findIndex((x) => x.index === defIdx);
                if (at < 0) at = 0;
                if ((list || []).length) sel.selectedIndex = at;
            };
            fill(this.el.mic, d.capture, d.defaults && d.defaults.mic, pm);
            fill(this.el.ref, d.loopback, d.defaults && d.defaults.ref, pr);
            fill(this.el.out, d.playback, d.defaults && d.defaults.out, po);
            this.setDot(this.el.dotMic, (d.capture || []).length > 0);
            this.setDot(this.el.dotRef, (d.loopback || []).length > 0);
            this.setDot(this.el.dotOut, (d.playback || []).length > 0);
            const names = (d.playback || []).map((x) => x.name).join('\n');
            this.el.cablewarn.hidden = /CABLE/i.test(names);
            this.savePrefs();
        } catch (e) {
            this.el.err.textContent = 'devices: could not reach server (' + e.message + ')';
        }
    }

    resetDefaults() {
        // Client-side "Reset to defaults": device defaults from the last
        // enumeration, 100% mic level, both post stages on.
        const d = this.lastDevices;
        const pick = (sel, list, defIdx) => {
            const at = (list || []).findIndex((x) => x.index === defIdx);
            sel.selectedIndex = at >= 0 ? at : 0;
        };
        if (d) {
            pick(this.el.mic, d.capture, d.defaults && d.defaults.mic);
            pick(this.el.ref, d.loopback, d.defaults && d.defaults.ref);
            pick(this.el.out, d.playback, d.defaults && d.defaults.out);
        }
        this.el.gain.value = 100;
        this.showGain();
        this.el.ns.checked = true;
        this.savePrefs();
        this.el.status.textContent = this.running ? this.el.status.textContent : 'Defaults restored - press Start.';
    }

    async toggleStart() {
        this.savePrefs();
        const btn = this.el.start;
        btn.disabled = true;
        btn.textContent = this.running ? 'Stopping…' : 'Starting…';
        try {
            const r = this.running
                ? await this.api('POST', '/api/stop')
                : await this.api('POST', '/api/start');
            if (!r.ok) {
                this.el.err.textContent = r.error || 'failed';
            } else {
                this.el.err.textContent = r.state.error || (r.state.ns && r.state.ns.error) || '';
                this.setRunning(r.state.running, r.state);
            }
        } catch (e) {
            this.el.err.textContent = 'server unreachable (' + e.message + ')';
            this.setRunning(false, {});
        } finally {
            btn.disabled = false;
            this.paintStartBtn();
        }
    }

    paintStartBtn() {
        this.el.start.textContent = this.running ? ('Stop   ' + this.uptime()) : 'Start';
        this.el.start.classList.toggle('button-danger', this.running);
    }

    uptime() {
        if (!this.running) return '';
        const s = Math.floor((Date.now() - this.startStamp) / 1000);
        return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
    }

    setRunning(on, st) {
        this.running = on;
        st = st || {};
        if (on) this.startStamp = Date.now();
        this.paintStartBtn();
        this.setDot(this.el.dot, on);
        this.el.runstate.textContent = on ? ('Running  ' + this.uptime()) : 'Idle';
        this.el.runstate.style.color = on ? '#7be294' : '';
        // Devices (and the output reroute) can't change mid-stream
        // (desktop BeginDisabled).
        for (const s of [this.el.mic, this.el.ref, this.el.out]) s.disabled = on;
        this.el.rescan.disabled = on;
        this.el.listen.disabled = on;
        this.el.levels.hidden = !on;
        if (!on) {
            this.el.devhint.textContent = '';
            this.el.status.textContent = 'Stopped.';
        } else {
            this.el.status.textContent = 'Running (16000 Hz, ' + (st.chain || 'DTLN-AEC') + ')';
        }
    }

    meter(canvas, v, key) {
        const g = canvas.getContext('2d');
        const w = canvas.width, h = canvas.height;
        const lvl = Math.min(1, Math.max(0, v * 4));
        this.peak[key] = Math.max(this.peak[key] * 0.92, lvl);
        g.clearRect(0, 0, w, h);
        const bw = Math.floor(lvl * w);
        g.fillStyle = lvl > 0.8 ? '#c04545' : (lvl > 0.6 ? '#e09a3c' : '#2a9d5c');
        if (bw > 0) g.fillRect(0, 0, bw, h);
        const px = Math.floor(this.peak[key] * w);
        g.fillStyle = 'rgba(255,255,255,0.7)';
        g.fillRect(Math.min(px, w - 2), 0, 2, h);
    }

    applyWallpaper(save) {
        const v = this.el.wp.value;
        if (save) { try { localStorage.setItem('ig-wallpaper', v); } catch (e) {} }
        const b = document.body.style;
        if (v === 'slate') b.background = 'linear-gradient(160deg,#1b2430,#10151c)';
        else if (v === 'abyss') b.background = 'radial-gradient(1200px 600px at 20% 0%,#12332a,#0d141b 70%)';
        else if (v === 'ember') b.background = 'radial-gradient(1200px 600px at 80% 0%,#3a2222,#141017 70%)';
        else b.background = '';
    }

    connect() {
        let ws;
        try {
            ws = new WebSocket('ws://' + location.host + '/ws');
        } catch (e) {
            this.el.status.textContent = 'Server disconnected — retrying…';
            setTimeout(() => this.connect(), 1000);
            return;
        }
        this.ws = ws;
        ws.onopen = () => { this.wsRetry = 0; this.loadDevices(); };
        ws.onclose = () => {
            this.setRunning(false, {});
            this.el.status.textContent = 'Server disconnected — retrying…';
            setTimeout(() => this.connect(), Math.min(5000, 500 * Math.pow(2, this.wsRetry++)));
        };
        ws.onmessage = (ev) => {
            const st = JSON.parse(ev.data);
            if (st.running !== this.running) this.setRunning(st.running, st);
            if (this.running) {
                this.el.runstate.textContent = 'Running  ' + this.uptime();
                this.paintStartBtn();
            }
            if (st.nsEnabled !== undefined && !!+st.nsEnabled !== this.el.ns.checked)
                this.el.ns.checked = !!+st.nsEnabled;
            if (st.listenToSelf !== undefined && !!st.listenToSelf !== this.el.listen.checked) {
                this.el.listen.checked = !!st.listenToSelf;
                this.showListen();
            }
            if (st.micGain !== undefined) {
                const pct = Math.min(200, Math.max(0, Math.round(+st.micGain * 100)));
                if (pct !== +this.el.gain.value) { this.el.gain.value = pct; this.showGain(); }
            }
            if (!st.running) return;
            this.meter(this.el.mIn, st.inRms || 0, 'in');
            this.meter(this.el.mOut, st.outRms || 0, 'out');
            this.el.status.textContent =
                'Running (16000 Hz, ' + (st.chain || 'DTLN-AEC') + ') · NS ' + ((st.ns && st.ns.backend) || '?') +
                (st.ns && st.ns.dropped ? ' · ring drops ' + st.ns.dropped : '');
            if (st.error || (st.ns && st.ns.error))
                this.el.err.textContent = st.error || (st.ns && st.ns.error);
        };
    }
}

function initWebImGui() {
    if (webImGuiUI) {
        try {
            const saved = localStorage.getItem('ig-wallpaper');
            if (saved && webImGuiUI.el.wp) {
                webImGuiUI.el.wp.value = saved;
                webImGuiUI.applyWallpaper(false);
            }
        } catch (e) {}
        return webImGuiUI;
    }
    webImGuiUI = new WebImGuiUI();
    webImGuiUI.init();
    try {
        const saved = localStorage.getItem('ig-wallpaper');
        if (saved && webImGuiUI.el.wp) {
            webImGuiUI.el.wp.value = saved;
            webImGuiUI.applyWallpaper(false);
        }
    } catch (e) {}
    console.log('Web ImGui UI initialized successfully');
    return webImGuiUI;
}

// Public API (kept compatible with earlier builds).
window.WebImGui = {
    init: initWebImGui,
    setTab: (tab) => webImGuiUI && webImGuiUI.setTab(tab),
    setRunning: (on, st) => webImGuiUI && webImGuiUI.setRunning(on, st || {}),
    setMicGain: () => {},
    setNSToggled: () => {},
    setTrayEnabled: () => {},
    toggleDebug: () => {},
};

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => initWebImGui(), { once: true });
} else {
    initWebImGui();
}
