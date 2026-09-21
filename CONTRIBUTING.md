# Contributing to TFM

TFM is early-stage software (see README). A GitHub Actions CI workflow
(`.github/workflows/ci.yml`) runs on every push to `main` and every
pull request: it builds `tfm` under `-Werror`, runs the automated test
suite (`tests/`, currently covering `src/fileops.c`, `src/tfm_common.c`,
`src/config.c`, `src/input.c`, and `src/dir.c`) plus the slower PTY
integration tests, builds `tfm-gui`, runs the ASan/UBSan test suite, and
runs `cppcheck`. CI doesn't cover the GUI's own behavior or anything
needing a real display/Hyprland session, so manual verification still
matters more than usual for `src_gui/` changes.

## Building

```
make            # tfm (terminal UI, no dependencies beyond a C11 compiler)
make tfm-gui    # tfm-gui (needs gtk4 + libadwaita-1 development packages)
make test       # unit-test (see below) + a smoke test: builds and checks --version/--help
make unit-test  # just the automated tests/test_*.c suite
make test-pty   # slower: spawns real bin/tfm in a pty for signal/EOF/terminal-restore tests
make lint       # cppcheck, if installed
make asan       # tfm rebuilt with -fsanitize=address,undefined
make asan-test  # unit-test rebuilt with -fsanitize=address,undefined
```

## Before submitting a change

- Build both `tfm` and `tfm-gui` with zero warnings
  (`-Wall -Wextra -Wpedantic`, the default flags), and keep `make test`
  and `make asan-test` passing.
- For anything touching `src/fileops.c`, `src/tfm_common.c`,
  `src/config.c`, `src/input.c`, or `src/dir.c`: add or extend the
  matching `tests/test_*.c` case (see `tests/test.h` for the harness
  and `tests/test_fs_helpers.h` for shared `/tmp`-tree/env-var helpers)
  as well as testing against a real, disposable directory tree under
  `/tmp` by hand - not just by inspection. Cover at least the Retry/
  Skip/Abort paths you touched.
- For anything touching signal handling, startup/shutdown ordering, or
  raw-mode/terminal-restore behavior in `src/main.c` or `src/input.c`:
  add or extend a `tests/pty_test_main.c` case (see its header comment
  for the pty-draining gotcha - a real, disposable pty via `make
  test-pty`, not just the pipe-based `tests/test_input.c`) as well as
  manual verification.
- For anything touching `src_gui/`: launch `tfm-gui` for real (with an
  isolated `$HOME` if it touches config/theme state) and confirm the
  change visually before/after.
- Run `make asan` and exercise the changed path at least once.

## Scope

Open an issue describing what you found and how you verified it.

## Style

- No comments explaining *what* code does — only *why*, when the reason
  isn't obvious from the code itself (a workaround, an invariant, a subtle
  edge case).
- Prefer explicit error handling over silently swallowing a return value;
  this codebase treats "ignored return value" as a real defect class, not
  a style nit.
- Keep the TUI (`src/`) and GUI (`src_gui/`) front-ends behaviorally
  consistent where they overlap (confirmation dialogs, overwrite
  semantics, error messages) - divergence between them has been a
  recurring source of bugs found only in one front-end.
