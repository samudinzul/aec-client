AEC Client v%%VERSION%%

Highlights since v%%PREV%%:
%%HIGHLIGHTS%%

Install:
1. Install VB-CABLE (https://vb-audio.com/Cable/, free) and reboot.
2. Download AEC-Client-v%%VERSION%%-win64.zip from Assets below and extract it anywhere.
3. Run aec_gui.exe, pick your devices, click Start.
4. In Discord: input = CABLE Output, Input Profile = Voice Isolation.
   (Zoom/Teams: input = CABLE Output; turn off their echo cancellation.)

> SmartScreen notice: this app is unsigned (no paid code-signing certificate), so Windows may show
> "Windows protected your PC" on first launch. This is expected: click More info -> Run anyway.
> Every release is built straight from public source (https://github.com/samudinzul/aec-client) —
> audit it, rebuild it, or scan the ZIP on VirusTotal if unsure.

Full changelog in CHANGELOG.md.
Compare: https://github.com/samudinzul/aec-client/compare/v%%PREV%%...v%%VERSION%%