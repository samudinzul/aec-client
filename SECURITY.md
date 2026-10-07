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
- `nkf_smoke.exe` stays a console app (no `-mwindows` games).

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
- `CHANGELOG.md` is the audit trail: what changed, in which
  version, and why it is safe.

## Reporting

False positive on a release? File an issue with the engine name,
the detection label, the file hash (`sha256sum` is printed by both
release scripts), and the VirusTotal link. Do not "fix" it with a
packer, an obfuscator, or a renamed extension — those feed the
heuristics, they don't starve them.
