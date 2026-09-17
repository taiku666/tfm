# Changelog

TFM does not yet do versioned releases (see `TFM_VERSION` in
`include/tfm_common.h`, currently `0.1.0` for both binaries) - this file
tracks notable changes by theme rather than by release tag until that
changes.

## Unreleased

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

## Initial release

- Dual-panel terminal UI (`tfm`) and optional GTK4/libadwaita GUI
  (`tfm-gui`) sharing one UI-agnostic core.
- Copy/move/delete/mkdir/rename with Skip/Overwrite/Abort conflict
  handling and progress reporting.
- `$EDITOR` launch by file extension, shell command bar with `cd`
  built-in, persisted per-panel path/theme config.
- Live Omarchy theming for the GUI front-end.
