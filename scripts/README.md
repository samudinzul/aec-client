# scripts/ — build, test, release workflow

Run everything from the **repo root** in MSYS2 UCRT64 bash.

## Layout

- **Tools** (this folder): `make-desktop-build.sh`,
  `make-web-build.sh`, `test.sh`, `make-release-desktop.sh`,
  `make-release-web.sh`, `standardize-releases.sh` — plus this
  file, the only `.md` here, which documents them.
- **`templates/`**: release *inputs*, not docs — end-user README
  sources (`README-desktop.txt`, `README-web.txt`) and the GitHub
  notes template (`release-template.md`). The release scripts stamp
  versions into these; never edit the stamped outputs in `release/`
  (gitignored) — edit the templates.

## Daily loop

```bash
scripts/make-desktop-build.sh --fresh   # wipe + rebuild aec_gui.exe only (smoke tools need --with-smoke)
scripts/test.sh                        # offline tests (native + web)
```

## Full command reference

| Script | Purpose | Typical use |
|--------|---------|-------------|
| `make-desktop-build.sh` | Configure + build the app only — clean output is the default (no smoke tools, no stray logs). `--fresh` wipes `build/` first; `--debug` switches MinSizeRel → Debug (orthogonal flags) | `scripts/make-desktop-build.sh --fresh` · `--run` builds then launches the app · `--with-smoke` also builds `nkf_smoke.exe` (for `test.sh --native-only`) · `-v` streams full tool output |
| `make-web-build.sh` | Prepare + verify the web runtime (venv, packages, compile, engine smoke — no compiler involved) | `scripts/make-web-build.sh` · `--fresh` rebuilds `.venv` · `--lean` installs the base set only (DTLN profiles, ~140 MB) · `--run` builds then launches the GUI |
| `test.sh` | Offline test suite, no audio hardware needed | `scripts/test.sh` · `--native-only` / `--web-only` |
| `make-release-desktop.sh [ver]` | Stage + zip the desktop release (contract asserts included) | `scripts/make-release-desktop.sh` (version defaults to `APP_VERSION`) |
| `make-release-web.sh [ver]` | Stage + zip the web release (separate asset) | `scripts/make-release-web.sh` |
| `standardize-releases.sh` | Align GitHub release titles/notes (needs `gh auth login`) | `scripts/standardize-releases.sh` |

`templates/` holds the inputs (`release-template.md`,
`README-desktop.txt`, `README-web.txt`): the release scripts stamp
versions into them — don't edit the outputs (`release/`,
gitignored), edit these sources.

## What test.sh checks

- **native** (`build/nkf_smoke.exe --synth 10`): exit 0, `no trim`
  verdict, zero backstop `ATTACK` lines in a fresh `nkf-phase.log`.
  These expectations are specific to the synth (a real howl *should*
  attack) — they pin the false-trigger fix, not the protection.
- **web** (`python -m web.test_offline --smoke`, prefers `.venv`):
  synthetic pair through every available engine, expects `SMOKE OK`.
  Skips (not fails) when models/litert are missing.
- **web-res** (`python -m web.test_nkf_res`): NKF residual-suppressor
  transparency / echo gain / doubletalk fingerprint.
- **web-loop** (`python -m web.test_nkf_loop`): closed-loop no-blowout,
  brake engagement + release, give-up braking, escalation,
  sliding-drift boundary (L1–L8), and the
  loud-voice-no-false-trim guard.
- **web-notch** (`python -m web.test_nkf_notch`): tracking-notch
  stable-tone kill + sweep tracking + release, voice never engages
  (N1–N2). Skips when the NKF backend is unavailable.

Failures print where to look (`/tmp/nkf_smoke_out.txt`,
`/tmp/web_smoke_out.txt`, `/tmp/web_res_out.txt`,
`/tmp/web_loop_out.txt`, `/tmp/web_notch_out.txt`, `nkf-phase.log`).
