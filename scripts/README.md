# scripts/ — build, test, release workflow

Run everything from the **repo root** in MSYS2 UCRT64 bash.

## Daily loop

```bash
scripts/build.sh --clean --app-only   # build aec_gui.exe, lean, no test tools
scripts/test.sh                        # offline tests (native + web)
```

## Full command reference

| Script | Purpose | Typical use |
|--------|---------|-------------|
| `build.sh` | Configure + build (MinSizeRel, stripped, `--gc-sections`, ccache, size audit) | `scripts/build.sh --clean` · `--app-only` skips `nkf_smoke.exe` · `--debug` for a debuggable build |
| `test.sh` | Offline test suite, no audio hardware needed | `scripts/test.sh` · `--native-only` / `--web-only` |
| `make-release.sh [ver]` | Stage + zip the desktop release (contract asserts included) | `scripts/make-release.sh` (version defaults to `APP_VERSION`) |
| `make-web-release.sh [ver]` | Stage + zip the web release (separate asset) | `scripts/make-web-release.sh` |
| `standardize-releases.sh` | Align GitHub release titles/notes (needs `gh auth login`) | `scripts/standardize-releases.sh` |

`release-template.md`, `README.txt`, `README-web.txt` are inputs:
the release scripts stamp versions into them — don't edit the
outputs (`release/`, gitignored), edit these sources.

## What test.sh checks

- **native** (`build/nkf_smoke.exe --synth 10`): exit 0, `no trim`
  verdict, zero backstop `ATTACK` lines in a fresh `nkf-phase.log`.
  These expectations are specific to the synth (a real howl *should*
  attack) — they pin the false-trigger fix, not the protection.
- **web** (`python -m web.test_offline --smoke`, prefers `.venv`):
  expects `SMOKE OK`. Skips (not fails) when models/litert are
  missing.

Failures print where to look (`/tmp/nkf_smoke_out.txt`,
`/tmp/web_smoke_out.txt`, `nkf-phase.log`).
