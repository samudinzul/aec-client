# scripts/ — build, test, release workflow

Run everything from the **repo root** in MSYS2 UCRT64 bash.

## Layout

- **Tools** (this folder): `make-desktop-build.sh`,
  `make-web-build.sh`, `test.sh`, `make-release-desktop.sh`,
  `make-release-web.sh` — plus this file, the only `.md` here,
  which documents them.
- **`_common.sh`**: shared usage/flag helpers, sourced by every tool
  (never run directly). Implements the conventions below.
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

Rebuilding the experimental fork after an `exp/` edit — never touches
`aec_gui.exe`, so the shipped official build stays exactly as released:

```bash
scripts/make-desktop-build.sh --exp        # build aec_exp.exe only
scripts/make-desktop-build.sh --exp --run  # build + launch it
```

## Flag conventions

Every tool follows the same vocabulary and error contract:

| Flag | Meaning | Tools |
|------|---------|-------|
| `-h`, `--help` | print the tool's header usage and exit 0 | all |
| `-v`, `--verbose` | stream full output / trace every command | all |
| `--fresh` | wipe generated state first (`build/`, `.venv`, caches) | desktop + web build |
| `--run` | after building, launch the app | desktop + web build |
| `--exp` | select the experimental app (`aec_exp.exe`), never builds `aec_gui.exe` | desktop build |
| `--with-smoke` | **add** the offline smoke tools (`nkf_smoke`, `dtln_smoke`, `aec3_smoke`) to the app build | desktop build |
| `--native-only` | native suite only | `test.sh` |
| `--web-only` | web suites only | `test.sh` |
| `--` | end of flags; later args are positional | all |
| `[version]` | release version, positional (default `APP_VERSION`) | release tools |

- `--help` is generated from each tool's own header comments — edit
  the header, never a line range.
- Unknown flags and extra arguments always print
  `unknown flag: X (see --help)` (or `too many arguments: ...`) and
  exit **2**. Runtime failures exit **1**; success exits **0**. CI can
  therefore tell a misuse from a failed build.
- `--exp` and `--with-smoke` are independent: `--exp` picks **which app**
  gets built, `--with-smoke` **adds tools** to that build. You normally
  do not need `--with-smoke` at all — only when you want to run
  `scripts/test.sh --native-only`.

```bash
scripts/make-desktop-build.sh                 # aec_gui.exe only
scripts/make-desktop-build.sh --exp           # aec_exp.exe only
scripts/make-desktop-build.sh --with-smoke    # aec_gui.exe + smoke tools
scripts/make-desktop-build.sh --exp --with-smoke  # aec_exp.exe + smoke tools
```

## What the scripts do

| Script | Purpose | Typical use |
|--------|---------|-------------|
| `make-desktop-build.sh` | Configure + build the app only — clean output is the default (no smoke tools, no stray logs). `--fresh` wipes `build/` first; `--debug` switches MinSizeRel → Debug (orthogonal flags); `--exp` builds only `aec_exp.exe` (experimental fork, `aec_gui.exe` untouched) | `scripts/make-desktop-build.sh --fresh` · `--run` builds then launches the app (`aec_exp.exe` with `--exp`) · `--with-smoke` also builds `nkf_smoke.exe` (for `test.sh --native-only`) · `-v` streams full tool output |
| `make-web-build.sh` | Prepare + verify the web runtime (venv, packages, compile, engine smoke — no compiler involved) | `scripts/make-web-build.sh` · `--fresh` rebuilds `.venv` · `--run` builds then launches the GUI · `-v` streams pip/compile output |
| `test.sh` | Offline test suite, no audio hardware needed | `scripts/test.sh` · `--native-only` / `--web-only` · `-v` dumps each suite's log tail |
| `make-release-desktop.sh [ver]` | Stage + zip the desktop release (contract asserts included) | `scripts/make-release-desktop.sh` (version defaults to `APP_VERSION`) · `-v` traces the staging/asserts |
| `make-release-web.sh [ver]` | Stage + zip the web release (separate asset) | `scripts/make-release-web.sh` · `-v` traces the staging/asserts |

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
