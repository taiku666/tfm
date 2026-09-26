# Changelog

Versioned via `TFM_VERSION` in `include/tfm_common.h` and git tags
(`vX.Y.Z`); this file tracks notable changes per release.

## 0.6.0

- **Enter opens the selection, F3 edits it (TUI and GUI).** Enter on a
  file now opens it in the desktop's default application for its type
  (image viewer, PDF reader, video player, ...) via GIO - `gio open` in
  the TUI, the GIO API directly in the GUI - so both front-ends pick the
  same apps as other file managers, and Terminal=true apps like nvim
  start in a new terminal window. The app runs detached (own session,
  output on /dev/null), so it never draws over the TUI and survives
  tfm's terminal closing. Executables are run instead, after a Run/Cancel
  confirmation (default Cancel), in the panel's directory; the TUI keeps
  their output on screen until a key is pressed. A file type
  with no default application shows an error popup. F3 opens any file in
  `$EDITOR`; the `[editor]` extensions list in `tfm.ini` is gone (an old
  file with it still loads, and the section is dropped on the next save).
- **Automated tests and CI.** Unit tests for fileops (with fault
  injection and a real cross-filesystem move), tfm_common, config, input,
  dir, panel, the Omarchy theme parser and the new opener, plus PTY
  integration tests that drive the real `tfm` binary. CI builds with
  `-Werror` and runs the tests, ASan/UBSan and scan-build.
- **Build hardening.** The default build is now `-O2 -D_FORTIFY_SOURCE=2`
  with `-Wconversion -Wshadow`, and `make scan` is a clean scan-build gate.
- **Fixes.** A race in the trash code that could overwrite an existing
  file; F6 Rename / F7 New folder accepting `/` or `..` and acting outside
  the current directory; a failed directory read mistaken for an empty
  trash; a panel showing a new path over its old listing when the new
  directory failed to load; `colors.toml` inline comments dropping the
  whole GUI theme; config load/save failures that were silent are now
  reported; fileops error messages include the system's reason; Home/End/
  PgUp/PgDn work in the TUI.
- **Internal.** The TUI's `main()` key loop is split into one handler per
  key, and code comments were trimmed to present-tense rationale.

## 0.5.0

- **Recoverable delete (trash/undo).** F8 now moves the selected item to
  the freedesktop.org home trash (`$XDG_DATA_HOME/Trash`, the same
  location GNOME Files/Dolphin use) instead of deleting it outright,
  writing standard `.trashinfo` metadata so it's restorable from those
  tools too. F9 (TUI) / Ctrl+Z or F9 (GUI) undoes the single most
  recently trashed item, restoring it to its original path - not a
  trash browser, and not undo for copy/move, just "undo my last
  delete." Shift+F8 bypasses the trash for a real, permanent,
  unrecoverable delete (e.g. for large files or sensitive data). Cross-
  filesystem trashing reuses the same copy-then-delete fallback
  `fileops_move()` already had. Verified with real `/tmp` fault-
  injection tests (collision-safe naming, empty-trash/overwrite-refusal
  edge cases, percent-encoding round-trip for paths with spaces/special
  characters) and a live Hyprland session covering all three key
  bindings end-to-end.

## 0.4.0

- Closed out nearly every remaining open Medium/Low finding from the
  project's code review passes: UTF-8 backspace (byte-vs-codepoint),
  a keystroke silently discarded when it arrived alongside a terminal
  resize, `fileops_move()` not reporting a failed source-side delete,
  NULL-pointer guards on shared helpers, a directory-recursion depth
  limit (stack-overflow safety), a permission-widening window during
  copy, `.ini` value escaping (a `\n` in a path no longer corrupts
  `tfm.ini`), copy now preserves ownership/timestamps, `shell.c` child
  hygiene (stopped children, EINTR busy-spin), lossy `cd` error
  messages, GUI close-request dialog stacking, escape-sequence
  handling (Home/End/PgUp/PgDn/Insert/Delete/Shift-Tab now recognized,
  `select()`-based timeout disambiguation, unmatched bytes replayed
  instead of dropped), locale-aware directory sorting (`setlocale()`
  was never called), `colors.toml`/Hyprland-config parsing edge cases,
  GUI panel selection now restored by name after a reload, and a
  dynamically-sized rename/mkdir prompt buffer.
- Investigated and disproved one long-standing "AdwDialog leaks a
  GObject reference" finding with a real weak-reference trace instead
  of leaving it unverified.
- Every fix verified with a real test (pty harness, `/tmp` fault
  injection, or a live Hyprland screenshot), not by inspection alone.
- Removed dangling references to the project's local, gitignored
  `CODE_REVIEW.md` from the public `CONTRIBUTING.md`/`SECURITY.md`.

## 0.3.0

- Two independent AI-assisted code review passes; all reported
  Critical/High-severity findings fixed and verified with real `/tmp`
  and live-Hyprland tests (symlink/TOCTOU safety, cross-device move data
  loss, config save corruption, GUI modal re-entrancy, signal handling,
  raw-mode restore, and more).
- Several Medium/Low findings from the same reviews fixed: redundant
  syscalls in `copy_file()`, a raced `ENOENT` treated as retryable in
  `delete_recursive()`, a Super/Hyper key leak into the GUI shell entry,
  a real-EOF spin in `input_read_key()`, and various dead-code/magic-
  number/duplicate-popup-frame cleanups.
- `--version`/`--help` added to both `tfm` and `tfm-gui`.
- `dir_color` made configurable (previously hardcoded to `"blue"`).
- Makefile: header-dependency tracking (`-MMD -MP`), `DESTDIR` support,
  `test`/`lint`/`asan` targets, distro-neutral dependency-install messages.

## 0.2.0 / 0.1.0

See the GitHub releases for these versions - predate this file.

## Initial release

- Dual-panel terminal UI (`tfm`) and optional GTK4/libadwaita GUI
  (`tfm-gui`) sharing one UI-agnostic core.
- Copy/move/delete/mkdir/rename with Skip/Overwrite/Abort conflict
  handling and progress reporting.
- `$EDITOR` launch by file extension, shell command bar with `cd`
  built-in, persisted per-panel path/theme config.
- Live Omarchy theming for the GUI front-end.
