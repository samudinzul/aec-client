# Security posture: zero false-positive tolerance

Rule zero of this project: **no build we ship may read as malicious
to any antivirus** — no virus/malware/trojan flags, no heuristic
detections, no SmartScreen warnings. Not "low risk": zero. Every
decision below follows from that rule.

## Threat model (what flagged us before)

- The unsigned desktop `.exe` bundles ONNX Runtime + TFLite and is
  rare (low prevalence). That profile trips ML heuristics:
  Microsoft `Wacatac.B!ml` and MaxSecure `susgen` (2/71 on
  VirusTotal). The bundled runtimes are legitimate; the *profile*
  (unsigned + uncommon + ML code) is what fires.
- Any PE binary we compile ourselves (`.exe`, `.dll`, `.pyd`) has
  the same problem: zero cloud reputation. Mainstream wheels
  (numpy, onnxruntime) do not — prevalence is its own reputation.

## Desktop build (`aec_gui.exe`) — reduced risk, not zero

Hardened as far as an unsigned binary can be; every item is
enforced, not advisory:

- No packers, ever (`scripts/make-build-desktop.sh`, `CMakeLists.txt`).
  UPX-packing trips the exact heuristics this project dodges.
- Symbols stripped, dead code GC'd (`-s`, `--gc-sections`, MinSizeRel).
- Full publisher metadata in `version.rc` (Company/Product/FileVersion
  + icon). An anonymous exe is the worst reputation profile; the
  desktop release script **fails the build** if the metadata version
  drifts from `APP_VERSION`.
- Embedded `asInvoker` manifest (`aec_gui.manifest`, version-locked
  by the same asserts): declares up front the app never elevates —
  no UAC prompt, no admin token, nothing for behavior monitors to
  question. Minimal on purpose (no DPI/compat declarations that
  would change rendering or lie to the OS).
- `nkf_smoke.exe` stays a console app (no `-mwindows` games).
- Clean API surface (audited, re-audit on every new Windows call):
  no sockets, no registry writes, no Run-key/autorun persistence,
  no process creation or injection, no debugger-evasion, no
  elevation requests. Audio + local files + tray icon only.
- User data (config, logs, crash dumps) stays next to the exe
  (portable-app layout). Never move it under system locations, and
  never write outside the app folder + `%TEMP%` — Controlled
  Folder Access and behavior monitors both key off write scope.
- Vendored DLLs ship byte-identical from the vendor. Never
  strip, repack, or "optimize" `libs/onnxruntime.dll` — modifying
  a signed vendor binary voids its signature and reads as
  tampering to every heuristic.

Residual risk, stated plainly: none of the above is code signing.
**A code-signing certificate (~$200/yr) is the only reliable fix**
for the exe.** Until then the exe carries nonzero flag risk, and
that is why the web build exists.

## Web build (`AEC-Web-vX-win64.zip`) — zero by construction

- Pure script + data only. `scripts/make-release-web.sh` asserts
  **twice** (staging + zip contents) that no `.exe`, `.dll`, `.pyd`,
  cache, or venv is present — the build fails otherwise. No
  exceptions, including for our own optional `aec_dsp` accelerator
  (local-only; `web/dsp.py` falls back to NumPy without it).
- Dependencies are mainstream PyPI wheels installed on the user's
  machine by the launchers. Never freeze with PyInstaller/Nuitka —
  that reintroduces the packed-binary target.
- Do not add a compiled component to the web bundle without
  updating this file and the release asserts first.

## Release hygiene

- Never `--clobber` a published release with different code: every
  version ships once, under a new tag, after the asserts pass.
- Before publishing, upload both zips to VirusTotal manually and
  record the verdict with the release notes. A single detection on
  a previously-clean build blocks the release until explained.
- A Defender/ML false positive on the exe is fought with process,
  not code tricks: submit the file to Microsoft Security
  Intelligence (filesubmission portal, "incorrectly detected as
  malware") and link the submission in the release notes. Cleared
  verdicts accumulate to the file's and publisher's reputation —
  this is the free, legitimate path that actually moves ML verdicts
  over releases. Never "fix" a detection with a packer, an
  obfuscator, or a renamed extension.
- End-user download note (both READMEs): right-click the zip →
  Properties → **Unblock** before extracting. A
  Mark-of-the-Web-tagged extraction makes every script launch
  ask SmartScreen first — unblocking at the zip keeps first-run
  clean. (Scripts themselves carry no AMSI-trigger strings:
  audited — no powershell, downloaders, registry, or encoded
  commands; just python/pip/curl-to-localhost.)
- `CHANGELOG.md` is the audit trail: what changed, in which
  version, and why it is safe.

## Reporting

False positive on a release? File an issue with the engine name,
the detection label, the file hash (`sha256sum` is printed by both
release scripts), and the VirusTotal link. Do not "fix" it with a
packer, an obfuscator, or a renamed extension — those feed the
heuristics, they don't starve them.
