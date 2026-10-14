// Dear ImGui-style Web UI — DOM implementation, no WebAssembly.
//
// Mirrors the desktop client (src/main.cpp DrawUI):
// app header with status dot + Running/Idle, a horizontal
// Audio / Soundboard / Appearance / About tab bar, and the same
// sections, labels and footer behavior. Same PROFILES table
// (DTLN-AEC 128 / WebRTC AEC3 / NKF-AEC) with the same
// experimental-engines gating; options whose backend is missing
// are disabled with a note. Fixed at 16 kHz like the desktop's
// neural engines.

'use strict';

let webImGuiUI = null;

const APP_VERSION = 'v1.13.1';

class WebImGuiUI {
    constructor() {
        this.running = false;
        this.startStamp = 0;
        this.ws = null;
        this.wsRetry = 0;
        this.inited = false;
        this.el = {};
        this.lastDevices = null;
        // Saved monitor device pref (null = follow Your speakers);
        // applied once loadDevices has filled the dropdown.
        this._prefMonitorDevice = null;
        // Saved mic/ref/out device prefs; applied once loadDevices
        // has filled the dropdowns (same race-safe pattern as the
        // monitor device above).
        this._prefDevs = null;
        // Debounce timer for slider-driven prefs saves.
        this._saveTimer = null;
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
        this.loadSounds();
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
        const TABS = ['audio', 'soundboard', 'appearance', 'about'];
        for (const name of TABS) {
            const b = this.mk('button', 'tab' + (name === 'audio' ? ' active' : ''),
                name[0].toUpperCase() + name.slice(1));
            b.type = 'button';
            tabs.appendChild(b);
            this.el.tabBtns[name] = b;
        }
        app.appendChild(tabs);

        this.el.panels = {};
        this.el.panels.audio = this.buildAudioPanel();
        this.el.panels.soundboard = this.buildSoundboardPanel();
        this.el.panels.appearance = this.buildAppearancePanel();
        this.el.panels.about = this.buildAboutPanel();
        for (const name of TABS) {
            if (name !== 'audio') this.el.panels[name].style.display = 'none';
            app.appendChild(this.el.panels[name]);
        }

        document.body.appendChild(app);
        this.el.app = app;
    }

    buildAudioPanel() {
        const p = this.mk('div');
        p.style.cssText = 'max-width:640px;margin:0 auto;';

        // --- Profile (same PROFILES table as the desktop) ---
        p.appendChild(this.sep('Profile'));
        const prof = this.mk('div', 'frow');
        prof.innerHTML = '<span class="flabel">Profile</span>' +
            '<select id="ig-profile">' +
            '<option value="0">DTLN-AEC 128</option>' +
            '<option value="1">WebRTC AEC3</option>' +
            '<option value="2">NKF-AEC</option>' +
            '</select>';
        p.appendChild(prof);
        const stages = this.mk('div');
        stages.innerHTML =
            '<div class="chkline" role="radiogroup" aria-label="Noise suppression">' +
            '<span class="flabel">Noise suppression</span>' +
            '<label><input type="radio" name="ig-ns" id="ig-nsOff" value="off">Off</label>' +
            '<label><input type="radio" name="ig-ns" id="ig-nsDtln" value="dtln" checked>DTLN-NS (default)</label>' +
            '<label><input type="radio" name="ig-ns" id="ig-nsHush" value="hush">Hush</label>' +
            '</div>' +
            '<div class="hint">Off: lowest CPU · DTLN-NS: general hiss · Hush: clicks + talkers.</div>' +
            '<div id="ig-nsHushHint" class="hint" hidden>Hush needs the weya_nc library + model bundle on this machine (see web/README.md) — DTLN-NS runs until then.</div>' +
            '<div id="ig-enginfo" class="hint"></div>';
        p.appendChild(stages);

        // --- Devices (Refresh top-right, dot + 190px label rows) ---
        const devHead = this.sep('Devices');
        devHead.innerHTML += '<button id="ig-rescan" type="button" class="button button-small button-secondary" style="margin-left:auto">Refresh</button>';
        p.appendChild(devHead);
        const rows = this.mk('div');
        rows.innerHTML =
            '<div class="frow"><span class="flabel"><span id="ig-dotMic" class="status-dot inactive"></span>Your microphone</span><select id="ig-mic"></select></div>' +
            '<div class="frow"><span class="flabel"><span id="ig-dotRef" class="status-dot inactive"></span>Your speakers</span><select id="ig-ref"></select></div>' +
            '<div class="frow" id="ig-rowOut"><span class="flabel"><span id="ig-dotOut" class="status-dot inactive"></span><span id="ig-outLabel">Send cleaned sound to</span></span><select id="ig-out"></select></div>' +
            '<div id="ig-cablewarn" class="warn" hidden>VB-CABLE not found - install VB-CABLE and enable CABLE Input, then hit Refresh.</div>' +
            '<div id="ig-devhint" class="hint" style="color:#ffd75e"></div>' +
            '<label class="chkline"><input type="checkbox" id="ig-listen">Listen to myself</label>' +
            '<div class="hint">Hear yourself through your speakers instead of sending to voice apps — for testing. ' +
            'Applies after Stop -&gt; Start; the Output row above picks the destination in both modes.</div>';
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
            '<div class="frow"><span class="flabel">Ref</span><canvas id="ig-mRef" class="meter" width="420" height="26"></canvas></div>' +
            '<div class="frow"><span class="flabel">Out</span><canvas id="ig-mOut" class="meter" width="420" height="26"></canvas></div>';
        p.appendChild(levels);
        this.el.levels = levels;

        const err = this.mk('div', 'hint');
        err.id = 'ig-err';
        err.style.cssText = 'color:#ff9d9d;white-space:pre-wrap';
        p.appendChild(err);
        return p;
    }

    buildSoundboardPanel() {
        const p = this.mk('div');
        p.style.cssText = 'max-width:640px;margin:0 auto;';

        // --- Level ---
        p.appendChild(this.sep('Level'));
        const lvl = this.mk('div', 'frow');
        lvl.innerHTML =
            '<span class="flabel">Soundboard gain</span>' +
            '<div class="fgrow" style="display:flex;gap:8px;align-items:center">' +
            '<input type="range" id="ig-sbGain" min="0" max="200" step="1" value="8" style="flex:1">' +
            '<span id="ig-sbGainVal" class="text-secondary" style="min-width:48px;text-align:right;font-size:13px">8%</span>' +
            '</div>';
        p.appendChild(lvl);
        p.appendChild(this.mk('div', 'hint',
            'Sounds play into the same stream as your mic, so voice apps hear both.'));

        // --- Monitor ---
        p.appendChild(this.sep('Monitor'));
        const mon = this.mk('div');
        mon.innerHTML =
            '<label class="chkline"><input type="checkbox" id="ig-monOn" checked>Play soundboard to my speakers</label>' +
            '<div class="hint">Sends a copy of the soundboard to a speaker device so you hear it locally. ' +
            'Voice apps still get it through VB-CABLE. Needs the session to be running.</div>';
        p.appendChild(mon);
        const monDev = this.mk('div', 'frow');
        monDev.innerHTML = '<span class="flabel">Monitor to</span>' +
            '<select id="ig-monDev"><option value="-1">Your speakers (follow)</option></select>';
        p.appendChild(monDev);
        const monVol = this.mk('div', 'frow');
        monVol.innerHTML =
            '<span class="flabel">Monitor volume</span>' +
            '<div class="fgrow" style="display:flex;gap:8px;align-items:center">' +
            '<input type="range" id="ig-monGain" min="0" max="200" step="1" value="30" style="flex:1">' +
            '<span id="ig-monGainVal" class="text-secondary" style="min-width:48px;text-align:right;font-size:13px">30%</span>' +
            '</div>';
        p.appendChild(monVol);
        const monHint = this.mk('div', 'hint');
        monHint.id = 'ig-monHint';
        p.appendChild(monHint);
        this.el.monHint = monHint;
        this.el.monWrap = monDev;
        this.el.monVolWrap = monVol;

        // --- Sounds ---
        const head = this.sep('Sounds');
        head.innerHTML += '<span style="margin-left:auto;display:flex;gap:8px">' +
            '<button id="ig-sbStopAll" type="button" class="button button-small button-danger" disabled>Stop all</button>' +
            '<button id="ig-sbRefresh" type="button" class="button button-small button-secondary">Refresh</button></span>';
        p.appendChild(head);
        this.el.sbCount = this.mk('div', 'hint');
        p.appendChild(this.el.sbCount);
        this.el.sbPads = this.mk('div');
        this.el.sbPads.style.cssText = 'display:flex;flex-wrap:wrap;gap:8px;margin-top:8px;';
        p.appendChild(this.el.sbPads);
        this.el.sbEmpty = this.mk('div', 'hint');
        p.appendChild(this.el.sbEmpty);

        // --- Transport (mirrors the Audio footer: same handlers, same
        // state — the desktop keeps transport + meters visible from
        // every tab, so the Soundboard tab gets its own copies) ---
        p.appendChild(this.sep(''));
        const sbFoot = this.mk('div', 'footer-btns');
        sbFoot.innerHTML =
            '<button id="ig-sbStartBtn" type="button" class="button">Start</button>' +
            '<button id="ig-sbResetBtn" type="button" class="button button-secondary">Reset to defaults</button>' +
            '<span id="ig-sbStatus" class="footer-status">Stopped.</span>';
        p.appendChild(sbFoot);

        const sbLevels = this.mk('div');
        sbLevels.id = 'ig-sbLevels';
        sbLevels.hidden = true;
        sbLevels.innerHTML =
            '<div class="sep">Live Levels</div>' +
            '<div class="frow"><span class="flabel">Mic</span><canvas id="ig-sbMIn" class="meter" width="420" height="26"></canvas></div>' +
            '<div class="frow"><span class="flabel">Ref</span><canvas id="ig-sbMRef" class="meter" width="420" height="26"></canvas></div>' +
            '<div class="frow"><span class="flabel">Out</span><canvas id="ig-sbMOut" class="meter" width="420" height="26"></canvas></div>';
        p.appendChild(sbLevels);
        this.el.sbLevels = sbLevels;
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
            '<label class="chkline"><input type="checkbox" id="ig-exp">Show experimental engines (WebRTC AEC3, NKF-AEC)</label>' +
            '<div class="hint">Off (default): the profile list shows DTLN-AEC 128 only. ' +
            'On: also shows WebRTC AEC3 and NKF-AEC — real engines, less tested than the default.</div>';
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
            '<li>Three processing profiles (DTLN / WebRTC AEC3 / NKF-AEC) + noise suppression (Off / DTLN-NS / Hush, 16 kHz)</li>' +
            '<li>Real-time processing with low CPU usage</li>' +
            '<li>Works with speakers, earphones, and headsets</li>' +
            '<li>Live level meters</li>' +
            '<li>Soundboard — play sounds over your voice, with transport + meters on its own tab</li>' +
            '<li>Native app window (Edge WebView2) with browser fallback</li>' +
            '</ul>';
        p.appendChild(f);
        p.appendChild(this.sep('Credits'));
        const c = this.mk('div');
        c.innerHTML = '<ul style="font-size:13px;padding-left:20px;margin:4px 0">' +
            '<li>DTLN-AEC — Westhausen &amp; Meyer (ICASSP 2021, MIT)</li>' +
            '<li>DTLN-NS — networkedaudio port of breizhn/DTLN denoise (MIT)</li>' +
            '<li>Hush — pulp-vision (Apache-2.0)</li>' +
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
        this.el.outLabel = q('ig-outLabel');
        this.el.gain = q('ig-micGain'); this.el.gainVal = q('ig-gainVal');
        this.el.nsOff = q('ig-nsOff'); this.el.nsDtln = q('ig-nsDtln');
        this.el.nsHush = q('ig-nsHush'); this.el.nsHint = q('ig-nsHushHint');
        this.el.listen = q('ig-listen');
        this.el.rowOut = q('ig-rowOut');
        this.el.profile = q('ig-profile');
        this.el.exp = q('ig-exp');
        this.el.enginfo = q('ig-enginfo');
        this.el.tray = q('ig-trayOn');
        this.el.start = q('ig-startBtn'); this.el.reset = q('ig-resetBtn');
        this.el.status = q('ig-status');
        this.el.sbStart = q('ig-sbStartBtn'); this.el.sbReset = q('ig-sbResetBtn');
        this.el.sbStatus = q('ig-sbStatus');
        this.el.err = q('ig-err'); this.el.devhint = q('ig-devhint');
        this.el.mIn = q('ig-mIn'); this.el.mRef = q('ig-mRef'); this.el.mOut = q('ig-mOut');
        this.el.sbMIn = q('ig-sbMIn'); this.el.sbMRef = q('ig-sbMRef'); this.el.sbMOut = q('ig-sbMOut');
        this.el.rescan = q('ig-rescan');
        this.el.cablewarn = q('ig-cablewarn');
        this.el.dotMic = q('ig-dotMic'); this.el.dotRef = q('ig-dotRef'); this.el.dotOut = q('ig-dotOut');
        this.el.wp = q('ig-wallpaper');
        this.el.sbGain = q('ig-sbGain'); this.el.sbGainVal = q('ig-sbGainVal');
        this.el.monOn = q('ig-monOn');
        this.el.monDev = q('ig-monDev');
        this.el.monGain = q('ig-monGain'); this.el.monGainVal = q('ig-monGainVal');
        this.el.sbStopAll = q('ig-sbStopAll');
        this.el.sbRefresh = q('ig-sbRefresh');
        this.el.padBtns = [];

        for (const name of ['audio', 'soundboard', 'appearance', 'about']) {
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
        [this.el.nsOff, this.el.nsDtln, this.el.nsHush].forEach((r) =>
            r.addEventListener('change', () => this.savePrefs()));
        this.el.profile.addEventListener('change', () => this.savePrefs());
        this.el.exp.addEventListener('change', () => {
            // The list rebuild (applyEngineAvailability) drops the
            // hidden options and falls back to DTLN by itself.
            this.savePrefs();
            this.applyEngineAvailability();
        });
        this.el.listen.addEventListener('change', () => {
            this.savePrefs();
            this.showListen();
            this.el.devhint.textContent = this.running ? 'Stop, then Start to apply the new output.' : '';
        });
        this.el.gain.addEventListener('input', () => { this.showGain(); this.queueSavePrefs(); });
        this.el.tray.addEventListener('change', () => this.savePrefs());
        this.el.start.addEventListener('click', () => this.toggleStart());
        this.el.reset.addEventListener('click', () => this.resetDefaults());
        this.el.sbStart.addEventListener('click', () => this.toggleStart());
        this.el.sbReset.addEventListener('click', () => this.resetDefaults());
        this.el.wp.addEventListener('change', () => this.applyWallpaper(true));
        this.$('ig-wpReload').addEventListener('click', () => this.applyWallpaper(true));

        this.el.sbGain.addEventListener('input', () => {
            this.el.sbGainVal.textContent = this.el.sbGain.value + '%';
            this.queueSavePrefs();
        });
        this.el.monGain.addEventListener('input', () => {
            this.el.monGainVal.textContent = this.el.monGain.value + '%';
            this.queueSavePrefs();
        });
        this.el.monOn.addEventListener('change', () => {
            this.savePrefs();
            this.showMonitor();
        });
        this.el.monDev.addEventListener('change', () => this.savePrefs());
        this.el.sbRefresh.addEventListener('click', () => this.rescanSounds());
        this.el.sbStopAll.addEventListener('click', () => {
            this.api('POST', '/api/soundboard', { action: 'stopAll' })
                .then(() => this.updatePads([]))
                .catch(() => {});
        });
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
        for (const k of ['audio', 'soundboard', 'appearance', 'about']) {
            this.el.tabBtns[k].classList.toggle('active', k === name);
            this.el.panels[k].style.display = k === name ? '' : 'none';
        }
    }

    showGain() {
        this.el.gainVal.textContent = this.el.gain.value + '%';
    }

    showListen() {
            // Desktop hides the Output row while Listen-to-myself is
            // on, on the theory that "showing it would suggest it
            // still does something". That reasoning assumes the
            // destination is obvious — it isn't: the web build
            // derives it from the ref loopback, and when that name
            // doesn't map to a playback endpoint the audio goes
            // nowhere and the user hears nothing with no clue why.
            // So: keep the row visible and relabel it. The destination
            // is now explicit and editable in both modes.
            this.el.rowOut.style.display = '';
            const lbl = this.el.outLabel;
            if (lbl) lbl.textContent = this.el.listen.checked
                ? 'Listen to myself — send to'
                : 'Send cleaned sound to';
        }

    applyEngineAvailability(engines) {
        // Desktop parity: extra profiles are HIDDEN until the
        // experimental flag is ticked (not greyed — the desktop
        // skips them in the combo entirely). A ticked-but-missing
        // backend stays visible yet disabled ("(unavailable)")
        // instead of failing at Start. Rebuilds only on change,
        // so the 10 Hz state ticks never disturb an open list.
        if (engines !== undefined) this._engines = engines;
        const exp = this.el.exp.checked;
        const names = ['dtln', 'aec3', 'nkf'];
        const labels = ['DTLN-AEC 128', 'WebRTC AEC3', 'NKF-AEC'];
        let html = '';
        for (let i = 0; i < 3; i++) {
            if (i !== 0 && !exp) continue;
            const missing = this._engines && this._engines[names[i]] === false;
            html += '<option value="' + i + '"' + (missing ? ' disabled=""' : '') + '>' +
                labels[i] + (missing ? ' (unavailable)' : '') + '</option>';
        }
        const sel = this.el.profile.value;
        if (this.el.profile.innerHTML !== html) this.el.profile.innerHTML = html;
        const stillThere = [...this.el.profile.options].some((o) => o.value === sel);
        this.el.profile.value = stillThere ? sel : '0';
    }

    showEngineInfo(st) {
        // NKF telemetry mirrors the desktop NkfState lines (delay
        // lock, exposure, notch/loop/RES trims, guard resets). Other
        // engines have no extra telemetry — the field stays empty.
        const box = this.el.enginfo;
        if (!st || !st.running || !st.nkf || !/NKF/.test(st.engine || '')) {
            box.textContent = '';
            return;
        }
        const k = st.nkf;
        const lock = k.locked ? ('locked d=' + k.lagSamples + (k.confident ? '' : ' (grace)')) : 'seeking';
        const expo = k.giveUp ? 'FAILED OPEN (mic passthrough)' : (k.exposed ? 'live' : 'warming up');
        let s = 'NKF: ' + lock + ' · ' + expo +
            (k.loopActive ? ' · loop!' : '') +
            (k.nocancelHot ? ' · no-cancel trim' : '') +
            ((k.loopDb !== undefined && k.loopDb < -0.5) ? ' · loop trim ' + k.loopDb.toFixed(1) + ' dB' : '') +
            ' · resets ' + k.guardResets +
            ((k.resDb !== undefined && k.resDb < -0.5) ? ' · RES ' + k.resDb.toFixed(1) + ' dB' : '');
        if (k.notchFreq > 0)
            s += ' · Notch ' + k.notchFreq.toFixed(0) + 'Hz ' + k.notchDb.toFixed(1) + 'dB';
        box.textContent = s;
        box.style.color = k.notchFreq > 0 ? '#ff9d6b' : '';
    }

    nsBackend() {
        // Radio group value: off / dtln / hush (default dtln).
        if (this.el.nsHush.checked) return 'hush';
        if (this.el.nsOff.checked) return 'off';
        return 'dtln';
    }

    setNsBackend(v) {
        this.el.nsOff.checked = v === 'off';
        this.el.nsDtln.checked = v === 'dtln';
        this.el.nsHush.checked = v === 'hush';
    }

    nsHasFocus() {
        const a = document.activeElement;
        return a === this.el.nsOff || a === this.el.nsDtln || a === this.el.nsHush;
    }

    showNsHint(st) {
        // Hush is user-supplied files, not a pip package: when the
        // library + bundle are absent the radio disables instead of
        // failing at Start (same pattern as engine availability).
        const avail = st && st.nsBackends;
        const missing = avail && avail.hush === false;
        this.el.nsHint.hidden = !missing;
        this.el.nsHush.disabled = !!missing;
    }

    savePrefs() {
        clearTimeout(this._saveTimer);
        return this.api('POST', '/api/prefs', {
            mic: this.prefVal(this.el.mic), ref: this.prefVal(this.el.ref),
            out: this.prefVal(this.el.out),
            nsBackend: this.nsBackend(),
            nsEnabled: this.nsBackend() !== 'off',
            micGain: Math.min(4.0, Math.max(0.1, (+this.el.gain.value) / 100)),
            trayEnabled: this.el.tray.checked,
            listenToSelf: this.el.listen.checked,
            profile: +this.el.profile.value,
            experimentalEngines: this.el.exp.checked,
            soundboardGain: (+this.el.sbGain.value) / 100,
            monitorEnabled: this.el.monOn.checked,
            monitorGain: (+this.el.monGain.value) / 100,
            // -1 = "Your speakers (follow)", the server's default
            monitorDevice: +this.el.monDev.value === -1 ? null : +this.el.monDev.value,
        }).catch(() => {});
    }

    queueSavePrefs() {
        // Slider `input` fires many times per second; coalesce into
        // one POST so rapid drags can't reorder on the wire.
        clearTimeout(this._saveTimer);
        this._saveTimer = setTimeout(() => this.savePrefs(), 120);
    }

    saveDevicePrefs() {
        // Device-only save: loadDevices calls this after an
        // enumeration so a vanished device snaps the server too,
        // without clobbering the non-device prefs that
        // refreshToggles may not have hydrated yet.
        return this.api('POST', '/api/prefs', {
            mic: this.prefVal(this.el.mic), ref: this.prefVal(this.el.ref),
            out: this.prefVal(this.el.out),
            monitorDevice: +this.el.monDev.value === -1 ? null : +this.el.monDev.value,
        }).catch(() => {});
    }

    refreshToggles() {
        this.api('GET', '/api/state').then((st) => {
            if (st && st.nsBackend !== undefined) this.setNsBackend(st.nsBackend);
            else if (st && st.nsEnabled !== undefined)
                this.setNsBackend(+st.nsEnabled ? 'dtln' : 'off');
            this.showNsHint(st);
            if (st && st.micGain !== undefined)
                this.el.gain.value = Math.min(200, Math.max(0, Math.round(+st.micGain * 100)));
            this.showGain();
        }).catch(() => this.showGain());
        this.api('GET', '/api/prefs').then((p) => {
            if (p && 'trayEnabled' in p) this.el.tray.checked = !!p.trayEnabled;
            if (p && 'listenToSelf' in p) this.el.listen.checked = !!p.listenToSelf;
            this.showListen();
            if (p && 'experimentalEngines' in p) this.el.exp.checked = !!p.experimentalEngines;
            if (p && 'profile' in p) this.el.profile.value = String(p.profile);
            if (p && p.soundboardGain !== undefined) {
                this.el.sbGain.value = Math.min(200, Math.max(0, Math.round(+p.soundboardGain * 100)));
                this.el.sbGainVal.textContent = this.el.sbGain.value + '%';
            }
            if (p && 'monitorEnabled' in p) this.el.monOn.checked = !!p.monitorEnabled;
            if (p && p.monitorGain !== undefined) {
                this.el.monGain.value = Math.min(200, Math.max(0, Math.round(+p.monitorGain * 100)));
                this.el.monGainVal.textContent = this.el.monGain.value + '%';
            }
            if (p && 'monitorDevice' in p) {
                this._prefMonitorDevice = p.monitorDevice;
                this.applyMonitorDevPref();
            }
            if (p) {
                // Restore the saved mic/ref/out selections once the
                // device dropdowns are filled (may land before or
                // after loadDevices — both orders must end right).
                this._prefDevs = { mic: p.mic, ref: p.ref, out: p.out };
                this.applyDevicePrefs();
            }
            this.showMonitor();
            this.applyEngineAvailability();
        }).catch(() => {});
        this.api('GET', '/api/state').then((st) => {
            this.applyEngineAvailability(st && st.engines);
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
            // Monitor device dropdown: follow-your-speakers + playback list.
            const md = this.el.monDev;
            md.innerHTML = '';
            const follow = document.createElement('option');
            follow.value = '-1';
            follow.textContent = 'Your speakers (follow)';
            md.appendChild(follow);
            (d.playback || []).forEach((x) => {
                const o = document.createElement('option');
                o.value = x.index; o.textContent = x.name; md.appendChild(o);
            });
            // Restore the pref (may have landed before or after this
            // enumeration — both orders must end on the right value).
            this.applyMonitorDevPref();
            // Same race-safe restore for the main mic/ref/out picks.
            this.applyDevicePrefs();
            // Device-only save: a vanished device snaps the server
            // too, without clobbering non-device prefs that
            // refreshToggles may not have hydrated yet.
            this.saveDevicePrefs();
        } catch (e) {
            this.el.err.textContent = 'devices: could not reach server (' + e.message + ')';
        }
    }

    applyMonitorDevPref() {
        const md = this.el.monDev;
        if (!md || !md.options.length) return;
        const pref = this._prefMonitorDevice;
        // null / -1 / missing = "Your speakers (follow)" (first option).
        const want = (pref === null || pref === undefined || pref === -1)
            ? '-1' : String(pref);
        md.value = want;
        if (md.selectedIndex < 0) { md.value = '-1'; }
    }

    applyDevicePrefs() {
        // Restore the saved mic/ref/out selections once the device
        // dropdowns are filled. A saved index that no longer exists
        // (device unplugged) leaves loadDevices' snapped default in
        // place — same contract as applyMonitorDevPref.
        const d = this._prefDevs;
        if (!d) return;
        const apply = (sel, idx) => {
            if (idx === null || idx === undefined) return;
            const want = String(idx);
            for (const o of sel.options) {
                if (o.value === want) { sel.value = want; return; }
            }
        };
        apply(this.el.mic, d.mic);
        apply(this.el.ref, d.ref);
        apply(this.el.out, d.out);
    }

    async loadSounds() {
        try {
            const s = await this.api('GET', '/api/sounds');
            this.renderSounds(s || { sounds: [], playing: [] });
        } catch (e) {
            this.el.sbEmpty.textContent = 'sounds: server unreachable (' + e.message + ')';
        }
    }

    async rescanSounds() {
        const btn = this.el.sbRefresh;
        if (btn.disabled) return;
        btn.disabled = true;
        try {
            const s = await this.api('POST', '/api/sounds/refresh');
            this.renderSounds(s || { sounds: [], playing: [] });
        } catch (e) {
            this.el.sbEmpty.textContent = 'sounds: server unreachable (' + e.message + ')';
        } finally {
            btn.disabled = false;
        }
    }

    renderSounds(s) {
        const list = Array.isArray(s.sounds) ? s.sounds : [];
        const playing = Array.isArray(s.playing) ? s.playing : [];
        const dec = Array.isArray(s.decoders) ? s.decoders : [];
        const sfOk = dec.indexOf('.mp3') >= 0;  // soundfile present
        this.sbPlaying = playing;
        this.el.sbCount.textContent = list.length
            ? list.length + ' sound' + (list.length === 1 ? '' : 's') + ' — ' +
              (s.dir || 'sounds/') + (sfOk
                  ? '' : ' · only .wav supported (install "soundfile" for mp3/flac/ogg)')
            : '';
        const extHint = sfOk ? '.wav/.mp3/.flac/.ogg' : '.wav only (install "soundfile" for more)';
        this.el.sbEmpty.textContent = list.length
            ? ''
            : 'No sounds found. Drop ' + extHint + ' files into "' + (s.dir || 'sounds/') + '" and press Refresh.';
        this.el.sbPads.innerHTML = '';
        this.el.padBtns = [];
        list.forEach((snd, i) => {
            const b = document.createElement('button');
            b.type = 'button';
            b.className = 'button';
            b.style.cssText = 'min-width:140px;text-align:left;padding:10px 12px;';
            b.textContent = (i + 1) + '. ' + (snd.name || ('sound ' + (i + 1)));
            b.disabled = !this.running;
            b.classList.toggle('pad-playing', playing.indexOf(i) >= 0);
            b.addEventListener('click', () => {
                this.api('POST', '/api/soundboard', { action: 'play', index: i })
                    .then((r) => {
                        // The server's `playing` is stale until the
                        // pump consumes the request (~8 ms); highlight
                        // optimistically so the pad lights instantly.
                        if (r && r.ok) this.updatePads([i]);
                    })
                    .catch(() => {});
            });
            this.el.sbPads.appendChild(b);
            this.el.padBtns.push(b);
        });
        this.updateStopAll();
        this.updatePads(playing);
    }

    updatePads(playing) {
        if (playing) this.sbPlaying = playing;
        const p = this.sbPlaying || [];
        this.el.padBtns.forEach((b, i) => {
            b.classList.toggle('pad-playing', p.indexOf(i) >= 0);
            b.disabled = !this.running;
        });
        this.updateStopAll();
    }

    updateStopAll() {
        this.el.sbStopAll.disabled = !((this.sbPlaying || []).length > 0);
    }

    showMonitor() {
        const en = this.el.monOn.checked;
        const listen = this.el.listen.checked;
        this.el.monWrap.disabled = !en;
        this.el.monVolWrap.style.opacity = en ? '1' : '0.5';
        this.el.monGain.disabled = !en;
        if (listen) {
            this.el.monHint.textContent =
                'Listen to myself already plays through your speakers. Monitor is off to avoid doubling.';
            this.el.monWrap.disabled = true;
            this.el.monVolWrap.style.opacity = '0.5';
            this.el.monGain.disabled = true;
        } else if (!this.running) {
            this.el.monHint.textContent =
                'Press Start to enable the local monitor.';
        } else {
            this.el.monHint.textContent =
                'Sends a copy of the soundboard to the selected speaker so you hear it locally.';
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
        this.setNsBackend('dtln');
        this.el.sbGain.value = 100;
        this.el.sbGainVal.textContent = '100%';
        this.el.monOn.checked = true;
        this.el.monGain.value = 100;
        this.el.monGainVal.textContent = '100%';
        this._prefMonitorDevice = null;
        this.applyMonitorDevPref();
        this.showMonitor();
        this.savePrefs();
        this.setStatus(this.running ? this.el.status.textContent : 'Defaults restored - press Start.');
    }

    async toggleStart() {
        // Await the prefs save so Start/Stop reads the latest device
        // and gain values — fire-and-forget raced the start POST.
        await this.savePrefs();
        const btns = [this.el.start, this.el.sbStart];
        for (const b of btns) {
            b.disabled = true;
            b.textContent = this.running ? 'Stopping…' : 'Starting…';
        }
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
            for (const b of btns) b.disabled = false;
            this.paintStartBtn();
        }
    }

    paintStartBtn() {
        const label = this.running ? ('Stop   ' + this.uptime()) : 'Start';
        for (const b of [this.el.start, this.el.sbStart]) {
            b.textContent = label;
            b.classList.toggle('button-danger', this.running);
        }
    }

    setStatus(t) {
        // Both transport footers (Audio + Soundboard tabs) show the
        // same status — desktop parity: transport is visible everywhere.
        this.el.status.textContent = t;
        this.el.sbStatus.textContent = t;
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
        if (on && !this._meterLoopOn) {
            this._meterLoopOn = true;
            this.micSmoothed = this.refSmoothed = this.outSmoothed = 0;
            this.micTarget = this.refTarget = this.outTarget = 0;
            requestAnimationFrame(() => this.meterLoop());
        }
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
        this.el.sbLevels.hidden = !on;
        // Soundboard pads only work while the session runs (mix happens
        // in the pump thread).
        this.el.sbRefresh.disabled = on;
        if (!on) {
            this.el.devhint.textContent = '';
            this.setStatus('Stopped.');
            this.updatePads([]);
            this.micSmoothed = this.refSmoothed = this.outSmoothed = 0;
            this.micTarget = this.refTarget = this.outTarget = 0;
        } else {
            this.setStatus('Running (16000 Hz, ' + (st.chain || 'DTLN-AEC') + ')');
        }
        this.showMonitor();
    }

    // Meter scale matches the desktop DrawLevelMeter(..., 3000.0f):
    // int16 RMS / 3000 == float RMS * 32768/3000 (~x11, full scale at
    // -21 dBFS). The old x4 needed -12 dBFS for full scale, so normal
    // speech parked at quarter height next to the desktop window.
    meterScale(v) {
        return Math.min(1, Math.max(0, v * 11));
    }

    // Render loop: redraws the meters every animation frame from the
    // smoothed values while running; WS ticks only move the targets,
    // so motion is 60 fps even though data arrives at ~30 Hz. Attack
    // snaps like the desktop's raw reads; release decays ~140 ms so
    // syllable gaps read instead of flicker. Started once from
    // setRunning; a missing canvas never kills the loop.
    meterLoop() {
        try {
            if (this.running) {
                const pairs = [
                    [this.el.mIn, 'micSmoothed', 'micTarget'],
                    [this.el.mRef, 'refSmoothed', 'refTarget'],
                    [this.el.mOut, 'outSmoothed', 'outTarget'],
                    [this.el.sbMIn, 'micSmoothed', 'micTarget'],
                    [this.el.sbMRef, 'refSmoothed', 'refTarget'],
                    [this.el.sbMOut, 'outSmoothed', 'outTarget'],
                ];
                for (const [c, sk, tk] of pairs) {
                    if (!c) continue;
                    const t = this[tk] || 0;
                    const v = this[sk] || 0;
                    this[sk] = t >= v ? t : v + (t - v) * 0.3;
                    this.drawMeter(c, this[sk]);
                }
            }
        } catch (e) { /* keep the loop alive */ }
        requestAnimationFrame(() => this.meterLoop());
    }

    drawMeter(canvas, v) {
        const g = canvas._ctx || (canvas._ctx = canvas.getContext('2d'));
        const w = canvas.width, h = canvas.height;
        g.clearRect(0, 0, w, h);
        const bw = Math.floor(v * w);
        g.fillStyle = '#2a9d5c';
        if (bw > 0) g.fillRect(0, 0, bw, h);
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
            this.setStatus('Server disconnected — retrying…');
            setTimeout(() => this.connect(), 1000);
            return;
        }
        this.ws = ws;
        ws.onopen = () => {
            this.wsRetry = 0;
            // Full resync: devices, sounds AND prefs — a server
            // restart resets prefs to defaults, so the UI must not
            // keep showing stale values after a reconnect.
            this.loadDevices();
            this.loadSounds();
            this.refreshToggles();
        };
        ws.onclose = () => {
            this.setRunning(false, {});
            this.setStatus('Server disconnected — retrying…');
            setTimeout(() => this.connect(), Math.min(5000, 500 * Math.pow(2, this.wsRetry++)));
        };
        ws.onmessage = (ev) => {
            const st = JSON.parse(ev.data);
            if (st.running !== this.running) this.setRunning(st.running, st);
            if (this.running) {
                this.el.runstate.textContent = 'Running  ' + this.uptime();
                this.paintStartBtn();
            }
            // Guard every WS-driven form update with activeElement:
            // the 30 Hz tick must not fight the user mid-drag/mid-click
            // (the slider was snapping back while being dragged).
            if (st.nsBackend !== undefined && st.nsBackend !== this.nsBackend()
                && !this.nsHasFocus())
                this.setNsBackend(st.nsBackend);
            if (st.listenToSelf !== undefined && !!st.listenToSelf !== this.el.listen.checked
                && document.activeElement !== this.el.listen) {
                this.el.listen.checked = !!st.listenToSelf;
                this.showListen();
            }
            if (st.profile !== undefined && String(st.profile) !== this.el.profile.value
                && document.activeElement !== this.el.profile)
                this.el.profile.value = String(st.profile);
            if (st.experimentalEngines !== undefined
                && document.activeElement !== this.el.exp)
                this.el.exp.checked = !!st.experimentalEngines;
            if (st.soundboardPlaying !== undefined) this.updatePads(st.soundboardPlaying);
            this.applyEngineAvailability(st.engines);
            this.showNsHint(st);
            this.showEngineInfo(st);
            if (st.micGain !== undefined && document.activeElement !== this.el.gain) {
                const pct = Math.min(200, Math.max(0, Math.round(+st.micGain * 100)));
                if (pct !== +this.el.gain.value) { this.el.gain.value = pct; this.showGain(); }
            }
            if (!st.running) return;
            // Targets only — the rAF meterLoop renders them at 60 fps.
            this.micTarget = this.meterScale(st.inRms || 0);
            this.refTarget = this.meterScale(st.refRms || 0);
            this.outTarget = this.meterScale(st.outRms || 0);
            this.setStatus(
                'Running (16000 Hz, ' + (st.chain || 'DTLN-AEC') + ')' +
                ((st.ns && st.ns.backend) ? '' : ' · NS off') +
                (st.ns && st.ns.dropped ? ' · ring drops ' + st.ns.dropped : ''));
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
