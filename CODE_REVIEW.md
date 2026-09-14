# TFM Code Review

Full-codebase review (terminal UI + GUI + shared core). Ranked by severity.
No fixes applied yet — this is a findings log.

## Pass 3 (2026-09-14) — bugs, optimization, architecture, style

Fresh independent pass over the current state of the code (Pass 1 and Pass 2 items already `[FIXED]` are not re-reported unless noted as a possible regression). Run as 5 parallel reviews split by module group (core, terminal control flow, terminal rendering/misc, GUI, plus one cross-cutting sweep over the whole tree), with every finding put through independent adversarial refutation votes (3 votes for critical/high, 2 for medium, 1 for low) before inclusion below — 1 of 36 raw findings were dropped as unconfirmed. Findings are merged and de-duplicated across reviewers. Each item tagged `[bug]` / `[optimization]` / `[architecture]` / `[style]`.

### Critical

1. **[FIXED] [bug] F6 same-directory "Rename" silently overwrites an existing file with zero confirmation, unlike every other destructive op in tfm** — `src/main.c:427-451`. When F6 (Move) is pressed while both panels show the same directory, main.c deliberately routes to a hand-rolled rename flow instead of fileops_move (because fileops_move takes a destination *directory*, and moving onto the same directory would resolve to a same-path no-op): `if (strcmp(active_panel->path, other_panel->path) == 0) { ... screen_prompt_text("Rename", new_name, sizeof(new_name)) ... path_join(old_path,...); path_join(new_path,...); } else if (rename(old_path, new_path) != 0) { tui_show_popup("Error", "Rename failed"); } else { panel_reload(...); }`. There is no `lstat`/`stat` existence check on `new_path`, and no call into the app's own overwrite-confirmation machinery (`tui_fileop_on_overwrite` / `screen_prompt_overwrite`) that every single copy/move/delete elsewhere in the codebase goes through. The only guard is `strcmp(new_name, entry->name) != 0` (only prevents renaming a file to its own current name). POSIX `rename(2)` atomically replaces an existing regular-file destination with no warning. Concrete scenario: a directory contains `draft.txt` and `notes.txt`; the user selects `draft.txt`, presses F6, and types `notes.txt` (a typo, muscle memory, or an intentional "replace" that the user expects to be prompted about, exactly as every copy/move/delete operation in this app prompts). `rename("draft.txt", "notes.txt")` succeeds silently, and the previous contents of `notes.txt` are gone forever with no popup, no confirmation, and no indication anything was destroyed. This is a common, everyday action (not an exotic symlink-alias or dangling-symlink edge case like the fileops.c bugs Pass 2 found) and produces the exact same class of irreversible data loss that fileops.c's `on_overwrite` callback infrastructure exists specifically to prevent. Fix: before calling `rename()`, `lstat(new_path, ...)` and route through the same `tui_fileop_on_overwrite`/`screen_prompt_overwrite` confirmation used everywhere else (or reuse `fileops_move` in a way that tolerates a rename-in-place).

2. **[FIXED] [bug] Pressing Escape in the Rename/New-Folder prompt permanently hangs tfm-gui** — `src_gui/gui_main.c:712-759,829,1167-1169,1261-1274`. prompt_text_dialog() (used by F6 in-place Rename and F7 New Folder) builds a raw AdwDialog and relies entirely on its own OK/Cancel button handlers to end the blocking nested loop: `on_prompt_ok`/`on_prompt_cancel` -> `prompt_dialog_respond()` -> `adw_dialog_close(state->dialog); g_main_loop_quit(state->loop);`. It never calls `adw_dialog_set_can_close(dialog, FALSE)` and never connects a handler to the dialog's own `closed`/`close-attempt` signals. But AdwDialog has an independent, always-available Escape-to-close path (per libadwaita's own docs: closable via Escape/system action unless can-close is FALSE, in which case close-attempt fires instead) — this file demonstrably knows about and correctly uses that flag elsewhere: gui_progress_show() (gui_main.c:829) explicitly sets `adw_dialog_set_can_close(dialog, FALSE)` so the copy/move/delete progress dialog can't be Escaped away mid-operation. prompt_text_dialog()'s dialog is left at the default can-close==TRUE; hiding the header bar's own close button (:721-722) does not disable the separate Escape mechanism. Nothing in on_window_key_pressed() intercepts Escape either: `if (g_modal_depth > 0) return GDK_EVENT_PROPAGATE;` (:1167-1169) is true for the whole duration of the dialog (gui_modal_enter() at :751), so the window-level controller deliberately lets the keypress reach the dialog's built-in handling. Concrete sequence: press F6/F7, then press Escape (the natural way to cancel a text prompt) instead of clicking Cancel. libadwaita closes/unmaps the dialog immediately, but `g_main_loop_quit(state.loop)` is only reachable from prompt_dialog_respond(), which the built-in Escape path never calls — `g_main_loop_run(state.loop)` at :752 never returns. action_move()/action_mkdir() and the whole call stack under whatever signal handler invoked them are now permanently stuck; gui_modal_leave() (:753) becomes unreachable, so g_modal_depth stays incremented forever. Every shortcut gated on `g_modal_depth > 0` (Tab, F5-F10, char-forwarding to the shell bar) is now dead for the rest of the process's life, and on_window_close_request() (:1261-1274) will refuse to let the window close ('An operation or dialog is still in progress...') — the only recovery is force-killing the process. Fix: set can-close FALSE and treat close-attempt as Cancel, or connect a 'closed' handler that calls g_main_loop_quit(state.loop) (guarded against double-invocation after an explicit button close).

Severity note: raised from the originally reported "high" to "critical" — this is a full, unrecoverable application hang (requiring a force-kill, with window-close itself refused) triggered by an extremely ordinary, expected interaction (pressing Escape to cancel a dialog), which makes it at least as severe as the F6 silent-overwrite data-loss bug in the TUI.

### High

3. **[bug] fileops_copy() lacks fileops_move()'s same-path guard, causing a per-file error-dialog storm when both panels show the same directory** — `src/fileops.c:465-496,117-125,599-602`. fileops_move() has an explicit early-exit right after computing dest:
```c
if (strcmp(src, dest) == 0) {
    report_error(cb, "Error", "Source and destination are the same file");
    return;
}
```
(fileops.c:599-602). fileops_copy() (fileops.c:465-496) computes `dest` the exact same way (strip_trailing_slashes + basename-join into dest_dir) but has NO equivalent check before calling copy_recursive(). The only thing that eventually catches src==dest is copy_file()'s per-file strcmp at line 117-125 — which fires once for EVERY regular file inside the copied tree, not once for the whole operation.

Concrete trigger: config_set_defaults() (config.c:37-52) sets BOTH left_path and right_path to $HOME, so on a fresh install/first run the two panels already point at the identical directory. If the user selects any subdirectory containing several files and presses F5 (copy) without having navigated the other panel elsewhere first, fileops_copy(src="$HOME/Documents", dest_dir="$HOME", cb) computes dest="$HOME/Documents" == src. dir_is_or_contains(dest_dir, src) does not fire (dest_dir is the parent of src, not a descendant), so copy_recursive(src, dest, ...) proceeds with identical paths. Directories silently "merge with themselves" via the mkdir()+EEXIST-is-a-directory branch (harmless no-op), but every single regular file at every depth hits copy_file()'s `strcmp(src_path, dest_path) == 0` branch and pops a full "Source and destination are the same file" Skip/Retry/Abort dialog — one dialog per file. For a folder with dozens or hundreds of files this makes the copy operation effectively unusable (the user must click through every single dialog, or eventually hit Abort partway through, in which case files processed before the abort point were already "processed" as same-file skips).

This refines Pass 2 item 18 (still open/unfixed), which described the missing guard as producing "a generic file-op error dialog" (singular) — in reality, for anything but a single top-level file, it's N dialogs, one per file in the tree, not one. The fix is a one-line addition mirroring fileops_move()'s existing check, placed in fileops_copy() right after `dest` is computed and before the self-containment/copy_recursive calls.

4. **[bug] copy_file()'s overwrite path calls bare unlink() with no directory check — defeats overwrite consent and produces an infinite Retry loop when the destination is a directory** — `src/fileops.c:127-172,194-207,265-273,342`. In copy_recursive(), a regular-file source is routed straight to copy_file() with no check on what the destination currently is: `if (!S_ISDIR(st.st_mode)) { return copy_file(src, dest, progress, cb); }` (fileops.c:341-342). copy_file()'s own conflict handling only distinguishes 'same inode' from 'needs an overwrite prompt' — it never checks whether the existing dest is a directory:
```c
FileOpChoice choice = report_overwrite(cb, dest_path);
if (choice != FILEOPS_CHOICE_OVERWRITE) {
    return choice == FILEOPS_CHOICE_SKIP;
}
unlink(dest_path);
```
(fileops.c:153-171). If `dest_path` is an existing DIRECTORY, `unlink()` fails with EISDIR/EPERM and the return value is completely discarded — unlike copy_recursive()'s analogous symlink-branch and mkdir()/EEXIST-branch, which route this exact situation (file/symlink over a type-mismatched destination) through remove_existing_for_overwrite() (fileops.c:265-273: `S_ISDIR(dest_st->st_mode) ? delete_recursive(dest, cb) : unlink(dest);`), per Pass 2 item 12's fix. copy_file()'s own overwrite path was never updated to call this same already-existing helper for the reverse type mismatch (file source, directory destination).

After the failed unlink, execution falls into the write loop with `dest_created == 0`, so `open(dest_path, O_WRONLY|O_CREAT|O_EXCL, 0666)` is used (fileops.c:194-195). Since the directory is still there, O_EXCL makes this fail with EEXIST, `out == NULL`, and the code reports a generic "Error writing" (fileops.c:202) — not "destination is a directory", not the errno text, and completely disconnected from the overwrite question the user just answered "Yes" to. If the user clicks Retry, the exact same sequence repeats indefinitely (unlink fails silently again, open fails again, same generic message again) since nothing about the directory ever gets removed; the only way out is Skip or Abort.

Concrete trigger: panel A has a file `report.txt`; panel B already contains a *directory* named `report.txt`. Pressing F5 shows "Already exists — Skip/Overwrite/Abort"; choosing Overwrite does not replace the directory — it just loops on a confusing "Error writing" message forever. This is also reachable whenever a copy encounters a same-named type conflict at the leaf-file level more generally (e.g., re-running a partially completed copy where a placeholder directory was created for what is a plain file in the source, or two trees that legitimately differ in entry type for a given name). No data is lost, but the user gets a confusing, seemingly-broken "Overwrite" action that silently does nothing and mislabels the actual cause.

Fix direction: before `unlink(dest_path)`, check `S_ISDIR(existing.st_mode)` and route through the existing `remove_existing_for_overwrite(dest_path, &existing, cb)` helper (already used elsewhere in this file for exactly this purpose), and check unlink()'s return value in the plain-file case too rather than assuming it always succeeds.

5. **[bug] Modal prompt redraws its box at a new size every frame without ever clearing the screen, leaving a stale border/content "ghost" from the previous frame** — `src/screen.c:963-1021,542-600,834-929`. screen_prompt_text() and screen_prompt_buttons() (and the draw_popup_frame() helper they/screen_draw_progress_popup() share) recompute start_row/start_col/box_width from scratch on every loop iteration and draw only the new box's cells - there is no screen_clear() or any erase of the previous box's footprint anywhere in this file (screen_clear() is defined in this file but is called exactly once in the whole program, from main.c's redraw_ui(), never from inside these modal loops). The doc comment above screen_prompt_buttons() even claims this makes it 'automatically resize-safe' ('Zeichnet bei jedem Tastendruck komplett neu - dadurch automatisch resize-sicher'), but 'redraw the new box' is not the same as 'erase the old box'.

For screen_prompt_text() this is trivially reachable with no terminal resize at all, through completely ordinary use: F6-triggered same-directory Rename pre-fills `edited` with the file's current name via `snprintf(new_name, sizeof(new_name), "%s", entry->name)` (main.c:432) and passes it to screen_prompt_text("Rename", new_name, ...). Inside the loop, `draw_popup_frame()` computes `inner_width` as `max(utf8_visual_width(title), utf8_visual_width(edited), utf8_visual_width(hint))` where `hint = "Enter=OK  Esc=Cancel"` is 20 columns wide. Renaming any file whose name is longer than 20 characters (extremely common, e.g. "budget-report-Q3-2026.xlsx") opens a wide box sized to the original name. The user then presses Backspace even once to shorten the name below 20 visible characters (`edited[--len] = '\0'` at screen.c:1005): on the very next loop iteration `inner_width` shrinks back down to the hint's 20-column width, `box_width` shrinks, and `start_col` moves right (box re-centers narrower) - but the previous, wider box's left-side border/content columns are never touched again, so they remain on screen as a disconnected strip of box-border characters immediately to the left of (and behind) the new, narrower box for the rest of the editing session, since the loop never clears and the caller's full-screen redraw only happens after Enter/Esc closes the dialog.

The same root cause also affects screen_prompt_buttons() (used for Overwrite/Skip-Retry-Abort/Yes-No confirmations) whenever a real terminal resize (SIGWINCH) occurs while it's open: `input_consume_resize_flag()` returning true just does `continue`, which redraws the box at the new rows/cols without any clear, so shrinking or growing the terminal mid-dialog leaves the old box's remnants visible around/behind the newly-positioned one.

Impact: visible, persistent screen corruption during a very common everyday action (renaming a file with a name longer than 20 characters and shortening it), not merely a rare resize edge case - and it contradicts the function's own doc comment describing this exact loop as resize-safe.

6. **[bug] screen_draw_progress_popup()'s percentage/bar row is the only row never passed through width clipping, so it overflows the popup's own right border on any moderately narrow terminal** — `src/screen.c:615-622,657-668,670-684,713-720,749-751`. Every other line in this popup (title at :741, item_display at :746) is emitted through print_utf8_padded(text, inner_width), which both truncates AND pads to exactly inner_width visible columns, so the trailing ` POPUP_VERTICAL` that closes each row always lands at the correct column. The percentage/bar row is built differently: `percent_line_padded` is only ever *padded* (screen.c:713-720: `for (int i = 0; i < pad_needed && ...) percent_line_padded[padded_len++] = ' ';` where `pad_needed = inner_width - percent_line_visual_width`), never truncated when `pad_needed` is negative - and then printed raw in one shot: `printf(POPUP_VERTICAL " %s%s" ANSI_RESET " " POPUP_VERTICAL, border_color_lookup(...), percent_line_padded);` (screen.c:750-751), with no move_cursor to reposition the closing border - it's just whatever comes next in the byte stream after percent_line_padded.

`pad_needed` goes negative whenever `percent_line_visual_width` (`bar_width + 2 brackets + 5 for the fixed " NNN%" suffix`, screen.c:668) exceeds `inner_width`, which is clamped to `max_inner_width = cols - 6` (floored at 10, screen.c:670-673) while `bar_width` is independently clamped only to `cols - 8` with its own floor of 5 (screen.c:615-622) - the two clamps are never reconciled. Concretely: bar_width stays at its default 30 for any cols >= 38, so for 38 <= cols <= 42 (a plausible narrow/tiled-terminal width, e.g. a split pane in a tiling WM), percent_line_visual_width = 30+2+5 = 37 while max_inner_width = cols-6 is only 32-36 - a guaranteed overflow. It never fully disappears even at the extreme minimum: bar_width floors at 5 and max_inner_width floors at 10, giving a minimum percent_line_visual_width of 5+2+5=12 against a maximum possible inner_width of 10 - so the row is *always* at least 2 columns wider than the box it's drawn inside, for cols below roughly 43.

Impact: on any terminal narrower than ~43 columns (common in tiled/split terminal setups), the progress popup shown during every copy/move/delete renders its percentage row wider than its own top/bottom border, pushing the row's closing `│` past the box's actual right edge and overlapping whatever is drawn immediately to its right (typically the file panel behind it) - a structurally misaligned, garbled progress display on an operation the user is specifically watching for correctness feedback.

7. **[bug] GUI shell-bar command execution freezes the entire window (no event pumping)** — `src_gui/gui_main.c:520,506-533 (cf. src/shell.c:11-72,74-76, src_gui/gui_main.c:392)`. on_shell_entry_activate()'s non-cd branch runs the typed command via `int exit_code = shell_execute(command, active->path);` (gui_main.c:520). shell_execute() (shell.c:74-76) is `shell_execute_cb(command, cwd, NULL, NULL)` — a NULL pump. Inside shell_execute_cb (shell.c:32-62), when pump==NULL the wait is a single blocking `waitpid(pid, &status, 0)` with no WNOHANG and no g_main_context_iteration() call anywhere in that path. Since tfm-gui is single-threaded, this blocks the GTK main thread for the command's entire duration: no redraw, no expose, no keyboard/mouse input processed — the window is reported 'Not Responding' by the compositor. This is exactly the class of bug Pass 1 item 18 fixed for `$EDITOR` launches ('editor_open() blocks the whole GTK main loop with no event pumping'), and the fix's plumbing already exists and is used correctly by on_item_activated() at gui_main.c:392 (`editor_open_cb(file_path, gui_pump_main_context, NULL)`) — but the GUI's own general-purpose shell/command bar, whose entire purpose is running arbitrary (sometimes slow) shell commands, was never switched to the pumping variant. Concrete trigger: type `sleep 5`, `git clone ...`, `tar xf big.tar.gz`, `npm install`, or any non-instant command into the `$` bar and press Enter — the whole tfm-gui window freezes solid for the command's duration. Fix: call `shell_execute_cb(command, active->path, gui_pump_main_context, NULL)` instead of `shell_execute()`, mirroring the already-fixed editor path.

8. **[bug] GUI panel_load() leaves the panel path empty on a failed initial listing, misdirecting operations to filesystem root, and the corruption is written back to tfm.ini on exit** — `src_gui/gui_main.c:76-84,126-153,155-182,370-401,405-450,1358-1361 (cf. src/tfm_common.c:5-8)`. GuiPanel.path (gui_main.c:77, in the zero-initialized static array `g_panel[2]` at line 84) is only ever assigned inside panel_load()'s success path: `if (dir_list(path, &entries, &count) != 0) { return; }` returns *before* the later `if (path != panel->path) snprintf(panel->path, ...)`. That early return is a safe 'keep the old, still-valid path' fallback after at least one prior success — but build_panel_widget() calls panel_load() for the very FIRST time on a freshly constructed GuiPanel, after already setting the visible label: `panel->path_label = gtk_label_new(initial_path);` (:408) then `panel_load(panel, initial_path);` (:447). If dir_list(initial_path,...) fails on this first call (a stale left_path/right_path in tfm.ini pointing at a now-unmounted USB drive/network share or a deleted directory — exactly the scenario Pass 2 item 4 fixed for the TUI's panel_reload/panel_init, but never touched here), panel_load() returns without ever assigning panel->path, and nothing else sets it later. The label keeps showing the intended (bad) path while panel->path is silently "" for the rest of the process's life, with an empty file list and no error shown anywhere.

Impact: every subsequent action on that panel joins paths via path_join() (tfm_common.c:5-8: `snprintf(out, out_size, "%s/%s", dir, name)`), so with dir=="" this always yields "/name" (filesystem root) instead of failing loudly. F5 Copy into the broken panel calls fileops_copy(src, other->path /* "" */, ...) targeting '/basename'; typing `cd tmp` in the shell bar while the broken panel is focused resolves via gui_builtin_cd()'s relative branch to '/tmp' (which usually exists), silently dropping the user into an unrelated system directory with zero indication their configured directory never loaded; F7 Mkdir attempts to create '/name' and surfaces a generic 'Permission denied' dialog that gives no hint the real problem is the panel's own corrupted state.

Worse, on_shutdown() (lines 1358-1359) unconditionally does `snprintf(g_cfg.left_path, ..., "%s", g_panel[0].path);` before config_save() — so the broken empty path is written back into tfm.ini as `left_path=`, and the next launch reads it back as "", calls dir_list("") (which fails the same way), reproducing the identical broken state: a self-reinforcing corruption that survives restarts until the user manually edits ~/.tfm/tfm.ini.

The TUI's equivalent code was deliberately hardened against exactly this (panel_init() sets panel->path unconditionally before the first panel_reload(), and panel_reload()'s failure branch synthesizes a '..' fallback entry so the panel stays defined and navigable) — panel_load() never received the equivalent treatment.

A second manifestation of the same silent-failure path: panel_navigate_into() (lines 155-182, called from on_item_activated() for any directory row) calls panel_load(panel, new_path) with no pre-check, unlike builtin_cd()/gui_builtin_cd() which explicitly opendir()-check and report 'Permission denied for this directory'. Double-clicking a directory row that is listed (DT_DIR) but not readable (another user's home directory, a systemd-private-* mount) makes dir_list() fail inside panel_load(), which just returns — nothing happens on screen, no dialog, no state change, no indication of why the click did nothing.

Fix: show an error when the initial load fails and/or fall back to a known-good directory (e.g. $HOME), updating panel->path and the label together, instead of leaving them silently out of sync; and never persist config from a panel state known to be invalid.

### Medium

9. **[bug] [possible regression] Directory permission preservation (Pass 2 item 3) is incomplete: mkdir() is still subject to umask, unlike copy_file()'s fchmod()** — `src/fileops.c:344-353`. Pass 2 item 3 ("File/directory permissions are never preserved on copy") is marked [FIXED]. For files, the fix is correct: copy_file() creates the destination with a fixed 0666 mode via open()/fdopen(), then explicitly forces the exact source permission bits afterward with `fchmod(fileno(out), src_mode_st.st_mode & 07777)` (fileops.c:233-236) — fchmod() is NOT subject to the process umask, so this genuinely reproduces the source's exact mode.

For directories, copy_recursive() instead does:
```c
if (mkdir(dest, st.st_mode & 07777) == 0) {
    break;
}
```
(fileops.c:351). Per POSIX, mkdir()'s mode argument IS masked by the process's umask before being applied — there is no follow-up chmod()/fchmod() call to force the exact bits the way copy_file() does for files. So a source directory whose mode includes bits that the running umask would strip is silently downgraded on copy: e.g. copying a `0777` or `0775` shared/group-writable directory under the common default `umask 022` produces a `0755` directory, silently losing the group-write/other-write bits that were the whole point of that permission setting (a shared team folder, a scratch/tmp-style directory, etc.). The specific example Pass 2 item 3 called out (a private 0700 directory) happens to survive because 0700 has no bits that a typical 022/077 umask would strip, which is presumably why the fix looked complete when tested against that example — but the general "permissions are preserved" claim in the surrounding comment (fileops.c:346-350, "Rechte der Quelle uebernehmen statt fest 0755") is not actually true whenever source-mode bits and the running umask overlap.

Fix direction: after a successful mkdir() (or after mkdir()+EEXIST-not-yet-a-dir path), call `chmod(dest, st.st_mode & 07777)` explicitly, mirroring copy_file()'s fchmod() approach, so directory permissions are as umask-independent as file permissions already are.

10. **[bug] dir_list() cannot distinguish a mid-read readdir() failure from normal end-of-directory, silently returning a truncated listing as success** — `src/dir.c:56-99`. dir_list()'s read loop is:
```c
struct dirent *entry;
while ((entry = readdir(dp)) != NULL) {
    ...
}
closedir(dp);
qsort(entries, count, sizeof(DirEntryInfo), compare_entries);
*out_entries = entries;
*out_count = count;
return 0;
```
Per POSIX, readdir() returns NULL both at genuine end-of-directory AND on error; the only way to tell them apart is to set `errno = 0` before the call and check `errno != 0` after a NULL return. This code does neither — any readdir() failure partway through the directory (e.g. EIO on a flaky NFS/FUSE/SSHFS mount, or ENOENT-class errors some filesystems surface if the directory is removed while being iterated) is silently treated as "we reached the end", and dir_list() returns 0 (success) with count set to however many entries were read before the failure.

This directly undermines the intent behind Pass 2 item 4, which was specifically about giving panels a reliable signal when a directory listing can't be trusted (e.g. an unmounted/deleted directory). Item 4's fix improved the opendir()-fails-outright case and the caller-side contract, but a failure that happens DURING the readdir loop (rather than at opendir()) still returns a clean-looking `0`/success with an incomplete entry list and zero indication to the caller that anything went wrong — exactly the "silently swallowed failure, panel shows fewer entries than actually exist, no error message" scenario item 4 was written to eliminate, just triggered one syscall later than the case that was actually fixed.

Fix direction: `errno = 0;` before the while loop, and after the loop exits, check `if (entry == NULL && errno != 0) { /* report/propagate failure */ }` before deciding whether to return 0 or -1 (with the already-collected entries either discarded or exposed as a partial/degraded result, per whatever contract dir.h's failure semantics settle on).

11. **[bug] tfm's own $ command line deletes a raw byte on Backspace, not a UTF-8 codepoint — a fresh call site for the (still-open) Pass 2 item 21 bug class** — `src/main.c:642-659`. `} else if (key.type == KEY_CHAR && (key.ch == 127 || key.ch == 8)) { if (cmd_len > 0) { cmd_buffer[--cmd_len] = '\0'; ... } }` (main.c:642-646) decrements the byte count cmd_len by exactly 1 regardless of whether the removed byte is a UTF-8 lead byte, a continuation byte, or plain ASCII. The character-append branch immediately below (main.c:647-659, the Pass 2 item 5 fix) explicitly appends raw UTF-8 bytes one at a time (`cmd_buffer[cmd_len++] = key.ch;`), so typing e.g. `cd Übung` at tfm's own prompt builds a valid multi-byte sequence in cmd_buffer. Backspacing right after such a character removes only its last byte, leaving a dangling lead byte (e.g. 0xC3 from ü = 0xC3 0xA9) in the buffer. screen_draw_command_line()'s utf8_visual_width()/print_utf8_padded() then miscount that dangling byte as a full column and print the truncated, invalid UTF-8 sequence straight to the terminal — the same rendering-corruption/cursor-offset bug class already logged (but only partially fixed, at the screen.c rename/mkdir prompt) as Pass 2 item 21, recurring here at a call site that fix never reached. If the user finishes typing and runs the command, the corrupted byte sequence is passed verbatim into is_cd_command()/builtin_cd() or shell_execute(), so a partially-backspaced non-ASCII argument can silently fail to match the intended file or directory.

12. **[bug] Unrecognized escape sequences are silently discarded in their entirety instead of falling back to individual keystrokes** — `src/input.c:143-178`. After the leading ESC byte, `input_read_key()` collects further bytes non-blockingly into `seq[8]` and, once collection stops (a known sequence matched, no more bytes arrived, or the 7-byte cap was hit), does: `if (seq_len == 0) { event.type = KEY_ESC; return event; } event.type = lookup_esc_sequence(seq, seq_len); return event;`. `lookup_esc_sequence` only recognizes the 20 entries in `ESC_SEQUENCES` (F1-F12 and the four arrow keys); anything else returns `KEY_UNKNOWN`, and the collected bytes in `seq[]` are never surfaced anywhere - not as a `KEY_ESC` event, not as the individual characters that followed. Concrete scenario: pressing Home (commonly `ESC[H]` or `ESC[1~`), End, PageUp/PageDown, Insert, Delete, or any Alt+letter combination most terminals encode as `ESC` followed by the letter (e.g., Alt+O sends `ESC O`, which is also a *prefix* of the recognized F1-F4 sequences `OP`/`OQ`/`OR`/`OS` and so is fully collected and then discarded when no further byte disambiguates it) results in the entire keypress vanishing with no feedback at all - the key does nothing, but unlike a genuinely unbound key, real input the user typed (the ESC and any keystroke bytes read from stdin) is consumed and thrown away rather than being reported as at least `KEY_ESC` (which the collection code already special-cases for the *empty*-sequence case at `seq_len == 0`) or replayed as ordinary `KEY_CHAR` bytes. A user relying on Home/End for quick command-line editing, or on Alt-key chords, gets silent, unexplained non-responsiveness with no way to tell whether the key was even received. Fix direction: on an unmatched non-empty `seq[]`, either buffer the trailing bytes for replay as ordinary `KEY_CHAR` events on subsequent calls, or at minimum still return `KEY_ESC` for the leading byte instead of `KEY_UNKNOWN` so higher layers have a defined event instead of silent data loss.

13. **[bug] config_save() is never called when tfm is terminated by SIGTERM/SIGHUP/SIGQUIT, unlike a normal F10 quit** — `src/main.c:269-285,673`. Pass 2 item 7 added `install_terminating_signal_handlers()` (main.c:275-285) specifically so a session logout / `kill <pid>` / WM exit restores the terminal cleanly via `exit(1)`, which runs the `atexit`-registered `input_disable_raw_mode`/`screen_leave_alt_screen`/`screen_show_cursor` handlers. However, `config_save(&cfg)` (main.c:673) is called only once, at the very end of `main()`'s normal `while (running)` loop exit path (i.e., only after F10/KEY_F10 sets `running = 0`) - it is never registered via `atexit()` and never invoked from `handle_terminating_signal()`. Concrete scenario: the user navigates both panels to new directories (updating `cfg.left_path`/`cfg.right_path` in memory, e.g. via Enter-navigation or a typed `cd`), then closes the terminal window, logs out, or is killed with `kill <pid>` from another terminal (exactly the scenarios item 7's own comment cites as the motivating cases) instead of pressing F10. The process exits cleanly (terminal state is restored, matching item 7's fix), but the session's visited-directory state is silently lost and the next launch reverts to whatever `tfm.ini` last had - a real behavioral difference between a signal-terminated exit and a normal quit that the terminal-restoration fix didn't extend to config persistence. (Calling `config_save()`, which uses `fopen`/`fprintf`, from an async-signal-unsafe context carries the same already-accepted risk the surrounding comment discusses for `exit()`, so a full fix likely wants a lightweight `sig_atomic_t`-triggered save-on-next-loop-check rather than calling it directly from the handler.)

14. **[bug] Editor launches from the GUI aren't gated by g_modal_depth, unlike every other blocking/pumped operation** — `src_gui/gui_main.c:370-401,894-1013`. Every other call in this file that pumps the main loop while blocking (fileops copy/move/delete via gui_fileop_on_progress; every alert/prompt dialog) is bracketed with gui_modal_enter()/gui_modal_leave() so on_window_key_pressed()'s `if (g_modal_depth > 0) return GDK_EVENT_PROPAGATE;` guard (:1167-1169) blocks F5-F8/F10/Tab from re-entering action handlers mid-operation — the file's own top comment (:89-97) states this is precisely why g_modal_depth exists. on_item_activated()'s editor-launch path is the one exception: `int exit_code = editor_open_cb(file_path, gui_pump_main_context, NULL);` (:392) pumps the main context exactly like gui_fileop_on_progress() does, but is never wrapped in gui_modal_enter()/gui_modal_leave(). While $EDITOR runs and this loop pumps GTK events, g_modal_depth stays 0, so on_window_key_pressed() treats the window as fully idle. Concrete scenario: open a text file with Enter (launches $EDITOR), then while it's still running press F8 on that same still-selected file. action_delete() fires immediately (its own confirmation dialog IS correctly gated), and on confirm calls fileops_delete() on the file the external editor still has open — deleting (or with F5/F6, copying/moving) it out from under the running editor session, since nothing here treats 'editor is open' as a modal state the way every other blocking operation in this file does. Fix: bracket the editor_open_cb() call with gui_modal_enter()/gui_modal_leave(), matching action_copy/action_move/action_delete's existing pattern around fileops_*.

15. **[bug] apply_omarchy_theme()'s early-return paths never revert a previously applied theme override** — `src_gui/gui_main.c:213-307,314-323`. apply_omarchy_theme() opens with `if (strcasecmp(g_cfg.gui_theme, "system") == 0) { return; }` and, further down, `if (!omarchy_theme_load(&colors)) { return; }`. Both skip every line that would touch adw_style_manager_set_color_scheme() or remove g_theme_css_provider. That's harmless the first time the function ever runs, but on_sigusr1() (:314-323) re-invokes this same function on every live theme-reload, and its own comment explains it reloads tfm.ini (not just colors.toml) specifically so 'eine zwischenzeitliche Aenderung von gui_theme (omarchy/system) erst nach einem Neustart wirken wuerde' would NOT happen — i.e. runtime-toggling gui_theme between omarchy and system via SIGUSR1 is an explicitly intended feature. It doesn't work in the omarchy -> system direction: if apply_omarchy_theme() previously forced a color scheme (:225-227) and installed g_theme_css_provider with custom @define-color/.tfm-panel-active overrides (:303-306), then the user sets gui_theme=system in tfm.ini and sends SIGUSR1, the function now takes the first early return and does nothing — the forced dark/light mode is never reset and the custom CSS provider is never removed from the display, so the window keeps looking exactly like the old Omarchy theme until the process restarts, directly contradicting the stated purpose of the SIGUSR1 tfm.ini reload. The same stale-override persistence happens via the second early return if the active Omarchy theme is deleted/renamed between reloads (omarchy_theme_load() starts returning 0), contrary to omarchy_theme.h:22-24's documented contract that callers should fall back to standard system theming in that case. Fix: before either early return, remove/unref g_theme_css_provider if non-NULL (as already done at :234-239 for the normal-reload case) and reset adw_style_manager_set_color_scheme() to ADW_COLOR_SCHEME_DEFAULT, so switching away from Omarchy theming at runtime actually takes effect.

16. **[bug] panel_reload()'s stale-listing fallback can show a new directory's path label over the previous directory's file list** — `src/panel.c:91-127,src/main.c:129-178,205-210`. panel_reload()'s Pass 2 item 4 fix intentionally keeps the panel's previous, still-valid `entries` when a reload's dir_list() fails, instead of blanking the panel — a sound tradeoff for the motivating 'flaky mount, same directory' case. But by the time panel_reload(panel) runs from enter_selected_entry(), panel->path (the very buffer builtin_cd() mutates in place, main.c:129 `char *current_dir`) has *already* been advanced to the newly-entered directory: builtin_cd() returns 1 and mutates current_dir before panel_reload() is called (main.c:205-210). builtin_cd() itself already validates the target with realpath()+stat()+opendir()/closedir() (main.c:154-174) before accepting it, so triggering this requires the target directory to become inaccessible in the narrow window between that check and dir_list()'s own opendir() inside the immediately-following panel_reload() — e.g. deleted or unmounted by another process at that exact moment. When that happens, panel->path now names the new (already-gone) directory while panel->entries (kept by the fallback) still lists the *previous* directory's contents, with no error and no visual difference from an ordinary successful navigation: the path header and the listed rows refer to two different directories. Acting on one of the stale rows (F5/F6/F8) harmlessly fails since the path built from panel->path doesn't exist, but until the next successful reload the displayed state is silently wrong.

### Low

17. **[optimization] Redundant stat()/lstat() syscalls on the same path within a single copy/move operation** — `src/fileops.c:136-137,233-236,286,609,635`. Several call sites re-derive information the caller already obtained one stack frame up, each costing an extra syscall per file/entry processed:
- copy_recursive() already calls `lstat(src, &st)` at fileops.c:286 to classify the entry (symlink/dir/file), giving full permission-bit and inode info for `src` — but copy_file() (called from copy_recursive() for every non-directory, non-symlink entry) never receives this `st`, and instead independently calls `stat(src_path, &src_st)` again at fileops.c:137 (only when dest already exists, for the inode-aliasing check) AND `stat(src_path, &src_mode_st)` again at fileops.c:234 (after every successful write, purely to get the mode for fchmod()) — up to two additional redundant stat() calls per file copied, on top of the one copy_recursive() already did.
- fileops_move()'s dest-exists branch calls `lstat(src, &src_lst)` at fileops.c:609 for the same-inode safety check, then calls `lstat(src, &src_st)` again at fileops.c:635 just to read `S_ISDIR` a few lines later — the exact same information is already sitting in `src_lst`.

None of these are individually expensive, but for a large tree (the codebase's own stated performance concern re: compute_total_size's double traversal, Pass 2 item 15) this compounds the per-file syscall count. Passing the already-known `struct stat`/mode down as a parameter (e.g. `copy_file(src_path, dest_path, &st, progress, cb)`) would eliminate the redundancy without any behavior change.

18. **[bug] config_load() doesn't validate that left_path/right_path are non-empty before overwriting the sane defaults** — `src/config.c:114-119,71-73;src/tfm_common.c:5-9`. config_load() calls config_set_defaults(cfg) first (config.c:73), which sets left_path/right_path to $HOME — a sensible fallback. But when tfm.ini has a `left_path=` or `right_path=` line whose value is empty or only whitespace (e.g. a manually edited/corrupted ini, or a value that trim() reduces to ""), the parser unconditionally overwrites the good default:
```c
if (strcmp(key, "left_path") == 0) {
    snprintf(cfg->left_path, sizeof(cfg->left_path), "%s", value);
}
```
(config.c:115-116, and symmetrically for right_path at 117-118) with no check that `value[0] != '\0'`. The resulting `cfg->left_path == ""` then feeds into `path_join(out, sz, dir="", name)` (tfm_common.c:5-9), which via `snprintf(out, out_size, "%s/%s", dir, name)` silently produces `"/name"` — an absolute, root-relative path completely disconnected from the user's intended directory — for any file operation that joins a selected entry onto that panel's path. In practice `opendir("")` itself fails first (so the panel would show an error/empty state per dir_list()'s contract), bounding the immediate damage, but the underlying gap — accepting and persisting an empty path value with no validation or fallback-to-default — is a real correctness hole in config_load() worth closing directly (e.g. skip the assignment, or fall back to the default, when `value[0] == '\0'`) rather than relying on downstream code to fail safely.

19. **[bug] fileops_copy()/fileops_move()'s normalized_src snprintf ignores truncation, unlike path_join() used elsewhere in the same file** — `src/fileops.c:467-469,578-580`. Both entry points normalize `src` the same way:
```c
char normalized_src[PATH_MAX];
snprintf(normalized_src, sizeof(normalized_src), "%s", src);
strip_trailing_slashes(normalized_src);
src = normalized_src;
```
(fileops.c:467-469 in fileops_copy, 578-580 in fileops_move). Unlike every other path-construction site in this file, which uses the shared `path_join()` helper and explicitly checks its truncation-detecting return value (e.g. fileops.c:409-417, 483-486, 594-597), this raw snprintf's return value is discarded. If the incoming `src` is longer than `PATH_MAX` bytes, `normalized_src` is silently truncated to a different, shorter path, and the rest of the function (basename extraction, the self-containment check, the actual copy/move) proceeds against that wrong, truncated path with no error reported to the caller. This mirrors the exact pattern already flagged elsewhere in the project for config.c (Pass 2 items 33/34) — an extreme edge case (a source path exceeding 4096 bytes) but the same silent-truncation class of bug, and trivially avoidable by checking the snprintf return the way path_join() already does two lines away in the same functions.

20. **[bug] config_save()/config_load() have no escaping for values containing the record separator ('\n'), corrupting round-trip on a path with an embedded newline** — `src/config.c:71-142,144-192`. Linux directory names may legally contain any byte except '/' and NUL, including a literal '\n', as well as '[', ']', '=', ';' and '#'. config_save() writes each field with a plain `fprintf(fp, "left_path=%s\n", cfg->left_path)` (config.c:171, similarly right_path at 172 and every other field) — a literal newline inside the path breaks the single logical value into two physical lines in tfm.ini right there, with zero escaping. config_load()'s line-oriented parser (fgets() + trim(), config.c:90-139) has no escaping/continuation support: on the next launch, the first half of the corrupted value is read back as a truncated left_path, and the second half — having no '=' — is silently skipped (`strchr(trimmed,'=')==NULL -> continue`, config.c:106-109); or, if the leftover text happens to start with '[', it is instead misparsed as a new INI section header, silently changing which subsequent keys apply, potentially clobbering a different config field or spilling into the next section.

This is the same general class of serialization/deserialization mismatch the project has already found and fixed once (Pass 1's config.c line-buffer-too-small bug); here the gap is that no field value is escaped/validated against containing the very separator character ('\n') the format uses, so a config value can corrupt config values written after it in the file. Concretely: navigating into a directory literally named e.g. 'weird<LF>name' and exiting tfm saves a corrupted, truncated left_path that resolves to a different (likely nonexistent) directory on the next launch, with no warning anywhere in the process. Low real-world likelihood, but a one-line INI writer with zero escaping for an unconstrained path string is a latent correctness gap worth noting.

21. **[bug] dir_list() still bypasses path_join() for the DT_UNKNOWN/DT_LNK stat() path (Pass 2 item 28, unfixed, confirmed still present)** — `src/dir.c:86-89`. Confirmed still present in the current code and within this pass's assigned scope, so re-flagged per the instructions (item 28 was left unfixed in Pass 2, items 16+). dir_list()'s stat-fallback branch for DT_UNKNOWN/DT_LNK entries builds the child path via a raw, unchecked snprintf:
```c
char full_path[PATH_MAX];
snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
struct stat st;
entries[count].is_dir = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
```
(dir.c:86-89), even though dir.c already `#include "tfm_common.h"` and could use the shared `path_join()` helper (which checks its own truncation return value) the way fileops.c consistently does. A `path` deep/long enough that `path + "/" + entry->d_name` exceeds PATH_MAX silently truncates here, and the subsequent stat() then runs against the wrong (truncated) path — most likely failing and defaulting `is_dir` to 0, misclassifying a symlinked directory as a plain file with no error surfaced anywhere.

22. **[bug] Progress-bar color is cached from the raw config value before the invalid-color-name fixup runs** — `src/main.c:300,337-367`. `screen_set_progress_bar_color(cfg.border_color)` is called at main.c:300, right after `config_load()`, and caches the raw string into screen.c's static `g_progress_bar_color` (`snprintf(g_progress_bar_color, sizeof(g_progress_bar_color), "%s", color_name)` at screen.c:141-144) with no validation. Only *afterwards*, at main.c:337-367, does the code check `screen_color_name_is_valid()` for `border_color`/`panel_border_color`/`text_color`/`cursor_color` and rewrite any invalid one to `"system"`, showing a "Config warning: Unknown color in tfm.ini: border_color (using system)" popup. That rewrite updates `cfg.border_color` (used everywhere else, e.g. `screen_draw_border(cfg->border_color, ...)` in every `redraw_ui()` call), but `screen_set_progress_bar_color()` is never called again with the corrected value. Concrete scenario: `tfm.ini` has a typo'd `border_color` (e.g. `boarder_color` value or an unrecognized name). The startup warning correctly tells the user it's been reset to "system" and the border immediately reflects that, but every subsequent F5/F6/F8 progress popup (`screen_draw_progress_popup`, which reads the cached `g_progress_bar_color` with no color argument of its own) keeps using the original invalid name for the rest of the session - `color_lookup()`'s fallback for an unrecognized name returns `""` (no color at all, per `screen.c:86`'s comment), so the progress bar silently renders without its intended accent color even though the user was told the problem was fixed. Fix: move the `screen_set_progress_bar_color()` call after the validation/fixup block, or re-call it whenever `cfg.border_color` is corrected.

23. **[bug] panel_reload()'s failure path resets scroll/selection to top even when it successfully preserves the old listing** — `src/panel.c:96-118`. Pass 2 item 4 was fixed by making `panel_reload()` keep the previous `panel->entries` on a `dir_list()` failure instead of destroying a valid listing (`src/panel.c:96-118`). However, both exit paths of that failure branch - whether a stale-but-valid `panel->entries` is kept as-is, or a fresh single-`".."`-entry fallback is allocated because `panel->entries` was `NULL` - fall through to the same unconditional `panel->scroll_offset = 0; panel->selected_index = 0; return;`. When the previous listing is genuinely preserved (the common case: a transient `opendir()` hiccup on an otherwise-still-valid, already-displayed directory - e.g. a flaky network mount or a permissions blip that clears up), the user's cursor and scroll position within that *unchanged* listing are needlessly discarded and jump back to the top, even though the entries themselves (and their indices) are still exactly what they were. This is a residual piece of exactly the state-destruction problem item 4 set out to fix, just for view state instead of data. Fix: only reset `scroll_offset`/`selected_index` in the fallback-allocation sub-case (where the old listing didn't exist to preserve indices for), not in the general preserved-old-listing case.

24. **[style] Directory listing color is hardcoded to "blue", bypassing the configurable/validated color system used everywhere else** — `src/panel.c:191-198`. `panel_draw()` computes `const char *entry_color = entry->is_dir ? "blue" : theme->text_color;` (panel.c:193). Every other themable color in the app (`border_color`, `panel_border_color`, `text_color`, `cursor_color`) is read from `Config`, validated at startup via `screen_color_name_is_valid()` (main.c:337-367, with a warning popup and fallback on typos), and passed through `PanelTheme`. Directory entries, however, are always rendered in a literal `"blue"` string with no `tfm.ini` knob to change it and no validation path at all (it can't be misconfigured because it isn't configurable). This is inconsistent with the project's own theming architecture - a user who sets a custom `text_color` (e.g. to match a light-background terminal where ANSI blue is hard to read) gets no equivalent control over directory-entry color, and any future refactor of the color-validation table needs to remember this separate, unlisted hardcoded color exists outside it.

25. **[style] F6 same-directory rename failure omits strerror(errno), unlike the analogous F7 mkdir failure** — `src/main.c:441-442,483-484`. F7's mkdir failure reports the specific OS error: `} else if (mkdir(new_dir_path, 0755) != 0) { tui_show_popup("Error", strerror(errno)); }` (main.c:483-484). The directly analogous F6 same-directory-rename failure a few dozen lines earlier instead shows a generic, fixed string with no detail: `} else if (rename(old_path, new_path) != 0) { tui_show_popup("Error", "Rename failed"); }` (main.c:441-442). Both are single-syscall failures in the same file, in the same general "prompt a name, then a plain filesystem syscall on it" pattern, but only one tells the user *why* it failed (e.g. `ENOTEMPTY` because the target name is a non-empty directory, `EACCES`, `ENOENT` because an intermediate path component moved, etc.). A user hitting the F6 rename failure has no way to distinguish a permissions problem from a name collision with a non-empty directory purely from the message shown.

26. **[style] One "Path too long" popup site still hand-rolls the hide/draw/wait/show pattern that tui_show_popup() was extracted to eliminate** — `src/main.c:504,509-511,523-524`. `tui_show_popup()` (main.c:99-105) exists specifically to replace "the same hide-draw-wait-show pattern that used to be duplicated at roughly a dozen call sites" (per its own doc comment), and every other "Path too long" popup in the file (F5 at :390, F6 at :440 and :455, F7 at :482) correctly uses it. The F8 delete handler's path_join failure branch does not: `screen_hide_cursor(); ... if (!path_join(target_path, ...)) { screen_draw_popup("Error", "Path too long"); input_wait_any_key(); } else { ... } screen_show_cursor();` (main.c:504,509-511,523-524) manually re-implements the same sequence inline instead of calling `tui_show_popup("Error", "Path too long")`. It happens to still work correctly here only because the surrounding F8 code already wraps the whole confirm+op block in its own `screen_hide_cursor()`/`screen_show_cursor()` pair, but it's a missed site from the original deduplication and a trap for future edits (e.g. if the outer hide/show pair is ever removed or restructured, this inner block silently stops showing the cursor again).

27. **[style] screen_draw_keybinding_bar() and screen_print_at() are fully implemented but never called anywhere** — `src/screen.c:254-263,432-437,include/screen.h:54,80`. Both functions are declared in the public screen.h API and fully implemented in screen.c, but a project-wide grep finds zero call sites for either. `screen_draw_keybinding_bar()` (screen.c:254-263, screen.h:54) has a doc comment describing its purpose ('Schreibt text in die Zeile direkt ueber dem unteren Rahmen') but `screen_draw_function_bar()` is what's actually used to draw the bottom bar in redraw_ui() (main.c:84) — this looks like a leftover from before the function-bar was introduced (both draw to the same `rows - 1` row). `screen_print_at()` (screen.c:432-437, screen.h:80) is similarly unused: every other on-screen text draw goes through screen_print_at_colored/screen_print_at_colored_bold/screen_print_at_selected — never the plain (uncolored) screen_print_at. Neither is incorrect, just unused surface area: both still get maintained/compiled and could confuse a future contributor into thinking one is the active mechanism for the bottom status line or plain text drawing — dead code in a project whose own CODE_REVIEW.md has already found and removed several other dead paths (the unused 'muted' theme field, a duplicated dead '.'-entry guard in main.c, a documented-but-unimplemented splash exit animation).

28. **[bug] build_big_text()/get_glyph() index the title string byte-by-byte, so any non-ASCII/multi-byte UTF-8 character would render as garbled per-byte glyphs instead of being handled per codepoint (latent - not reachable with the current hardcoded call site)** — `src/splash.c:71-112`. `build_big_text()` iterates `for (int i = 0; i < n; i++)` where `n = strlen(text)`, and calls `get_glyph(text[i])` treating each raw `char` as one on-screen glyph slot of fixed width FONT_W (splash.c:100-109). `get_glyph()` (splash.c:71-83) does `toupper((unsigned char)ch)` and a linear search of the ASCII glyph table, falling back to a single center-dot glyph for anything unmatched - so a UTF-8 continuation byte (which is what every byte after the first in a multi-byte codepoint looks like) would silently render as its own separate blank/center-dot glyph slot rather than being recognized as part of one character, corrupting the big block-letter rendering for any title/subtitle containing non-ASCII text (this is the same byte-vs-codepoint bug class already fixed multiple times elsewhere in this project, e.g. Pass 1 item 12 and Pass 2 items 5/21/48).

This is currently unreachable in practice: the only call site is `splash_show("TFM", "Taiku File Manager")` (main.c:294), both hardcoded ASCII literals, and splash.h documents the module as self-contained/copyable to other projects with no indication title is meant to be attacker- or user-controlled. Flagging as low/latent since it would only manifest if this reusable module were ever wired up to a configurable or localized title string.

29. **[architecture] builtin_cd() and gui_builtin_cd() duplicate ~40 lines of cd-resolution logic between the TUI and GUI** — `src/main.c:129-178,src_gui/gui_main.c:461-500`. builtin_cd() (main.c:129-178) and gui_builtin_cd() (gui_main.c:461-500) are near-byte-identical: the same three-way 'empty argument -> $HOME, absolute -> use as-is, relative -> current_dir/arg' raw-snprintf path construction, the same realpath() + stat()/S_ISDIR check, the same opendir()/closedir() readability probe with the same three error strings ('Directory not found', 'Not a directory', 'Permission denied for this directory'), and the same final `snprintf(current_dir, PATH_MAX, "%s", resolved);`. The project has already extracted equivalent shared logic once duplication was noticed (path_join() in tfm_common.c, replacing join_path/gui_join_path; other_panel_of()) but this pair was never folded into a shared helper, so a future fix to one copy (e.g. adopting path_join() instead of the raw snprintf at main.c:141,143 / gui_main.c:474,476 — Pass 2 item 37, still open) has to be remembered and reapplied by hand to the other.

30. **[optimization] GUI's shell-bar 'cd' reloads the active panel twice for one command** — `src_gui/gui_main.c:506-533`. In on_shell_entry_activate(): `if (gui_is_cd_command(command)) { ... else { panel_load(active, active->path); } } else { ... } ... panel_load(&g_panel[0], g_panel[0].path); panel_load(&g_panel[1], g_panel[1].path);`. For every `cd` typed into the GUI's shell bar, the just-navigated-to panel is reloaded once explicitly on success, then both panels are reloaded again unconditionally a few lines later regardless of which branch ran above. The active panel therefore gets a full dir_list() (fresh opendir+readdir+stat-per-entry) performed twice back-to-back for a single keystroke-driven action, doubling its syscall volume for no benefit — a sharper instance of the already-logged Pass 2 item 25 ('panel_load() always fully rebuilds ... called unconditionally ... regardless of whether it touched the filesystem').

### Cross-cutting architecture/style themes

- Overwrite/type-mismatch handling is inconsistent within fileops.c: some paths (fileops_move's same-path guard, copy_recursive's symlink/mkdir branches via remove_existing_for_overwrite()/delete_recursive()) were hardened in earlier passes, while their structural siblings (fileops_copy, copy_file's own overwrite branch) never received the equivalent guard — a repeated "fixed one path, forgot its twin" pattern that produced two of this pass's highest-severity findings.
- UTF-8 byte-vs-codepoint bugs keep recurring at independent call sites (main.c's own $ prompt Backspace, the earlier screen.c rename/mkdir prompt, and latently in splash.c's glyph renderer) with no shared "step back one codepoint" helper, even though utf8_visual_width() already exists as a shared width primitive.
- TUI fixes are not being propagated to the structurally identical GUI code path, and vice versa: Pass 2 item 4's panel-load hardening (never lose/blank a panel on a failed listing) was applied to the TUI's panel_init/panel_reload but never to the GUI's panel_load/panel_navigate_into; Pass 1 item 18's main-loop-pumping fix was applied to the GUI's editor-open path but never its shell-bar path; builtin_cd()/gui_builtin_cd() are ~40 lines of copy-pasted logic that will keep drifting apart.
- Modal/blocking-loop exit paths in both UIs assume there is exactly one way out: the TUI's screen.c prompt loops never clear the screen between frames (assuming redraw-in-place is always sufficient), and the GUI's AdwDialog-based prompts never account for the platform's own Escape-to-close bypassing the app's response callback — both are "the loop's exit condition can be triggered from somewhere the code didn't wire up" bugs.
- config.c's line-oriented INI format has two independently-found hardening gaps (silently accepting an empty path over a good default; no escaping of the '\n' record separator), and the GUI compounds this by writing a corrupted empty path back into tfm.ini on shutdown, creating a self-reinforcing corruption loop across restarts.
- Dead/unused API surface (screen_draw_keybinding_bar, screen_print_at) persists despite the project's own stated practice of proactively removing dead code once found, suggesting this cleanup pass is not yet systematic.

### Suggested fix order (Pass 3)

1. Fix the destructive-overwrite trio together — "F6 same-directory Rename silently overwrites an existing file", "fileops_copy() lacks the same-path guard", and "copy_file()'s overwrite path calls bare unlink() with no directory check" — these are the most reachable, highest-impact correctness bugs (irreversible data loss or an unusable copy operation) and all stem from missing confirmation/type checks on destructive filesystem ops, so audit and fix that logic in one pass.
2. Fix the two tfm-gui application-hang bugs together — "Escape in the Rename/New-Folder prompt permanently hangs tfm-gui" and "GUI shell-bar command execution freezes the entire window" — both leave the GUI completely unusable and are both event-loop/dialog-lifecycle issues local to gui_main.c.
3. Harden GUI panel initialization to match the TUI's existing fix — "GUI panel_load() leaves the panel path empty on a failed initial listing" — foundational because its self-reinforcing tfm.ini corruption compounds with every future launch until closed.
4. Fix the two screen.c popup-rendering bugs together — "Modal prompt redraws without ever clearing the screen" and "progress popup's percentage row overflows its own border" — same drawing subsystem, both visible during everyday copy/move/rename use.
5. Sweep the UTF-8 byte-vs-codepoint bug class across its known call sites — "tfm's own $ command line deletes a raw byte on Backspace" and the latent "splash.c glyph renderer" issue — by introducing one shared "back up one UTF-8 codepoint" helper rather than patching each site ad hoc, finally closing out the Pass 2 item 21 bug class.
6. Batch the remaining fileops.c/dir.c listing-and-copy correctness gaps — "directory permission preservation incomplete (umask)", "dir_list() cannot distinguish a readdir() failure from EOF", and "dir_list() bypasses path_join() for DT_UNKNOWN/DT_LNK" — independent medium/low fixes in the same file-listing/copy layer.
7. Harden config.c's parser in one pass — "config_load() doesn't validate non-empty left_path/right_path" and "config.c has no escaping for the '\n' record separator" — same file, same INI reader/writer.
8. Address the remaining GUI modal/state-consistency gaps together — "editor launches aren't gated by g_modal_depth", "apply_omarchy_theme() never reverts a previously applied theme", and "panel_reload()'s stale-listing fallback can show a mismatched path/entries pair" — all are "modal or reload state can go stale" bugs of similar severity.
9. Clean up the remaining low-priority style, dead-code and duplication items in a final hygiene pass — "screen_draw_keybinding_bar()/screen_print_at() dead code", "directory listing color hardcoded to blue", "F6 rename failure omits strerror(errno)", "F8 delete's hand-rolled Path-too-long popup", "builtin_cd()/gui_builtin_cd() duplication", and "GUI shell-bar cd reloads the active panel twice" — no urgency, batch together as codebase upkeep.

## Pass 2 (2026-09-13) — bugs, optimization, architecture, style

Fresh independent pass over the current state of the code (Pass 1 below is
fully `[FIXED]`). Run as four parallel reviews split by module group:
core (`fileops`/`dir`/`config`/`tfm_common`), terminal control flow
(`main`/`panel`/`input`), terminal rendering/misc (`screen`/`splash`/`shell`/
`editor`), and GUI (`gui_main`/`omarchy_theme`). Findings below are merged
and de-duplicated (a couple of bugs were independently caught by two
reviewers at different call sites of the same root cause). Not fixed yet —
findings log only. Each item tagged `[bug]` / `[optimization]` /
`[architecture]` / `[style]`.

### Critical

1. **[FIXED] [bug] Same-inode path aliasing silently truncates the source file to
   zero bytes on copy/move, with no way for the user to notice** —
   `src/fileops.c:96,107,133` (`copy_file`), and the dir+dir merge branch at
   `src/fileops.c:390,409-429` (`fileops_move`). The only "is this the same
   file" guard is `strcmp(src_path, dest_path)` — a pure string compare. If
   `src` and `dest` are different path *strings* that resolve to the same
   inode (e.g. the destination reached through a symlinked/bind-mounted
   directory alias — `~/Downloads` symlinked to `/mnt/data/Downloads`, and
   the two panels reach it via different spellings), `copy_file` opens `in`
   for read, then `fopen(dest_path, "wb")` **truncates that same inode to
   zero bytes**, and the already-open `in` handle then reads the
   now-empty file. The destination becomes 0 bytes; the original content is
   gone. The user sees a normal-looking "overwrite?" prompt with a
   *different-looking path*, so nothing warns them it's the same file. For
   `fileops_move`'s dir+dir merge branch it's worse: `copy_recursive`
   destroys every file this way and `delete_recursive(src, cb)` then
   deletes the (now-empty) source tree — total content loss. (Plain-file
   `rename()` fast paths elsewhere are unaffected — POSIX `rename()` on two
   aliases to the same file is a safe no-op.)

### High

2. **[FIXED] [bug] No protection against copying/moving a directory into its own
   descendant** — `src/fileops.c:280-299` (`fileops_copy`) and `:409-429`
   (`fileops_move` merge branch). Nothing checks that `dest` isn't nested
   inside `src`. `fileops_copy("/a/proj", "/a/proj/sub", cb)` computes
   `dest = "/a/proj/sub/proj"` and recurses: copying `sub` copies the
   newly-created `proj` right back into itself, one level deeper each pass,
   until `path_join()` starts failing at ~270 nested segments — exploding
   disk/inode usage first. GNU `cp`/`mv` both explicitly guard against this;
   tfm doesn't. (The plain `rename()` fast path is incidentally safe here —
   the kernel itself returns `EINVAL` — but the merge-existing-directory
   branch bypasses `rename()` and calls `copy_recursive` directly, so it's
   exposed.)

3. **[FIXED] [bug] File/directory permissions are never preserved on copy** —
   `src/fileops.c:133` (`copy_file`, no `chmod`/`fchmod` anywhere in the
   file) and `:229` (`mkdir(dest, 0755)` hardcoded regardless of source
   mode). Copying an executable script silently loses its execute bit;
   copying a `0600` file (e.g. an SSH private key) produces a world-readable
   `0644` copy under a typical `022` umask; a private `0700` directory
   becomes `0755`. Security-adjacent, not just cosmetic.

4. **[FIXED] [bug] `dir_list()` failures are silently swallowed by every caller,
   leaving panels in an inconsistent, un-escapable state** —
   `src/dir.c:34-39` returns `-1` on `opendir()` failure without touching
   `*out_entries`/`*out_count`; `panel_reload()`/`panel_init()`
   (`src/panel.c:80-101`) call it and ignore the return value entirely.
   Concretely: `tfm.ini`'s saved path points at an unmounted USB
   path/deleted directory at startup, or a directory is deleted/unmounted
   externally while displayed — the panel silently shows `count == 0` with
   **no `".."` entry**, indistinguishable from a genuinely empty directory,
   no error message, and no way to navigate out except Tab to the other
   panel. Reload also frees the previously-good listing *before* attempting
   the new read, so a transient failure (flaky mount) destroys valid data
   for nothing. Root-cause fix belongs in `dir_list()`'s contract (always
   define `*out_entries`/`*out_count` on failure); symptom-level fix belongs
   in both `panel.c` call sites.

5. **[FIXED] [bug] Recurrence of an already-fixed bug class: non-ASCII keystrokes
   are silently dropped in tfm's own command line** — `src/main.c:592`:
   `key.type == KEY_CHAR && key.ch >= 32 && key.ch < 127`. `KeyEvent.ch`
   (`include/input.h:29`) is a plain (signed) `char`; any UTF-8 lead/
   continuation byte (0x80–0xFF) becomes negative and always fails this
   gate. This is the exact bug already fixed in the rename/mkdir popup
   (Pass 1 item 12, `screen.c:945`) — a user can rename a file to "café" but
   can't type `cd Übung` at tfm's own `$` prompt. Best fixed at the type
   level (`KeyEvent.ch` should be `unsigned char`/`int`), since that's why
   it recurred at a call site the first pass didn't reach.

6. **[FIXED] [bug] Recurrence of an already-fixed bug class: stale SIGWINCH resize
   flag swallows the next real keystroke after `$EDITOR`, a shell command,
   or a long file operation** — `src/main.c:513-527` (editor branch),
   `:562-575` (shell branch), and the F5/F6/F8 `fileops_*` call sites
   (`:357-373,413-423,467-474`) with no intervening `input_read_key()`.
   Pass 1 item 13 fixed this for popups/prompts; the same gap exists at
   every "leave raw mode / run child or long loop / re-enter raw mode" call
   site in `main.c` — a resize during any of these leaves `g_resized` set,
   and the first real keystroke read afterward gets discarded as "just a
   resize."

7. **[FIXED] [bug] No `SIGTERM`/`SIGHUP`/`SIGQUIT` handler to restore the
   terminal** — `src/input.c:68-70` only registers
   `atexit(input_disable_raw_mode)`; same gap for
   `atexit(screen_leave_alt_screen)`/`atexit(screen_show_cursor)`
   (`src/main.c:257,295`). `atexit` handlers do not run on a signal-terminated
   process. A session logout, `kill <pid>` from another terminal, or WM exit
   leaves the shell in raw mode (no echo, no line editing, possibly still in
   the alternate screen buffer) until the user manually runs `stty
   sane`/`reset`. Notable since `input.c` already treats signal handling as
   a deliberate concern (it installs a `SIGWINCH` handler) but stops there.

8. **[FIXED] [bug] Menu bar / keybinding bar / command line never clip overlong
   text, unlike every other draw path in the file** —
   `src/screen.c:238-256,258-286,288-303`
   (`screen_draw_menu_bar`/`screen_draw_function_bar`/
   `screen_draw_command_line`). All three print with `printf("%-*s", cols-2,
   text)` — a field-width minimum, not a max; it never truncates. Every
   other text-drawing path in the file uses `print_utf8_padded`, which does
   truncate. `cmd_buffer` (`main.c:22`) is 256 bytes, easily longer than an
   80–120 col terminal width, so a shell command near/past terminal width
   overflows its row and wraps onto the row below, corrupting the fixed
   border/keybinding-bar layout. `screen_draw_command_line` additionally
   positions the cursor with `2 + (int)strlen(line)` — a byte count, not
   `utf8_visual_width()` — so any multi-byte UTF-8 character in the command
   puts the terminal cursor at the wrong column (same bug class as Pass 1
   item 11, missed here).

9. **[FIXED] [bug] `shell.c`'s pumped-wait loop mishandles `EINTR`, causing a false
   "command failed" report and a leaked/zombie child** —
   `src/shell.c:38-48`. The non-pumped branch correctly retries on
   `EINTR`; the `pump != NULL` branch treats *any* non-zero `waitpid()`
   return (including `-1`) as "child done" and returns immediately without
   checking `errno`, then reports `-1` (failure) while the child is still
   running and now unreaped. `input.c:76` installs `SIGWINCH` with no
   `SA_RESTART` specifically so reads get interrupted — so a terminal
   resize during a pumped shell command or `$EDITOR` session delivers
   exactly this spurious `EINTR`.

10. **[FIXED] [bug/architecture] No `close-request` handling on the GUI's main
    window — a WM/compositor-level close can destroy the window mid-file-op**
    — `src_gui/gui_main.c` (no `close-request` signal connected anywhere;
    `g_modal_depth`, used at `:1167-1169`, only gates the app's own
    `GtkEventControllerKey`, not the native window-close protocol). On
    Wayland/Hyprland, the default `SUPER+Q`/close-button path sends an
    xdg_toplevel close request straight to the surface, bypassing any
    `AdwDialog` grab (grabs are widget-input, not protocol-level vetoes).
    If this happens while `fileops_copy`/`move`/`delete`'s blocking progress
    loop (`gui_fileop_on_progress`, `:868-885`) or a nested
    `g_main_loop_run` alert/prompt (`:603-613,712-759`) is active, the
    window is destroyed underneath them: `gui_progress_hide()` then calls
    `adw_dialog_force_close()` on an already-torn-down widget, and pending
    dialog-response callbacks may hang (loop never quits) or dereference a
    dangling `g_window`.

### Medium

11. **[FIXED] [bug] Trailing slash on `src` breaks basename extraction, silently
    merging into the destination directory instead of creating a
    subdirectory** — `src/fileops.c:282-283,381-382`:
    `base = strrchr(src, '/'); base = base ? base+1 : src;` — if `src` ends
    in `/` (e.g. `"/home/user/dir/"`), `base` becomes the empty string and
    `dest` collapses to `dest_dir` itself. The op then merges `src`'s
    *contents* directly into `dest_dir`, silently colliding with any
    same-named entries already there, instead of creating `dest_dir/dir`.
    No trailing-slash normalization exists anywhere in the file.

12. **[FIXED] [bug/architecture] `copy_recursive` never routes symlink or
    type-mismatched entries through `on_overwrite`** —
    `src/fileops.c:196-222` (symlink branch calls `symlink()` directly; an
    existing `dest` fails `EEXIST` and is reported as a generic
    Skip/Retry/Abort error, never an overwrite choice) and `:228-237`
    (`mkdir`+`EEXIST` is always treated as "already a directory, fine," even
    when `dest` is actually a file/symlink — the next `opendir(dest)` then
    fails `ENOTDIR` with a confusing generic error). Retrying/re-running a
    partially-completed tree containing symlinks, or merging onto a
    type-mismatched destination, can't be resolved via the normal overwrite
    flow at all.

13. **[FIXED] [bug] `fileops_move`'s destination-exists check uses `stat()` instead
    of `lstat()`**, unlike `copy_file` (fixed in Pass 1 specifically for
    this reason) — `src/fileops.c:396`. A *dangling* symlink at `dest` makes
    `stat()` fail `ENOENT`, so the existence/overwrite branch is skipped
    entirely and `rename(src, dest)` silently replaces the dangling symlink
    with **no overwrite prompt**, contradicting `fileops.h:42-44`'s
    documented "asks before overwriting" contract.

14. **[FIXED] [bug] TOCTOU: the overwrite existence check uses `lstat` (no-follow)
    but the actual write uses plain `fopen` (follows symlinks)** —
    `src/fileops.c:107,133`, no `O_NOFOLLOW`/`O_EXCL`. If the "existing"
    entry the user agreed to overwrite is itself a symlink (or one is
    planted between check and write), the write silently lands wherever it
    points, potentially outside the intended destination directory.

15. **[PARTIALLY FIXED] [optimization] Full source tree is traversed twice
    per operation** — `compute_total_size()` (`src/fileops.c:57-109`) walks
    the entire tree purely for a progress-percentage denominator;
    `copy_recursive` then walks the identical tree again to actually copy.
    (The 3 call sites of `compute_total_size` — `fileops_copy` and
    `fileops_move`'s merge/EXDEV paths — are mutually exclusive per
    invocation, so this is a real 2x traversal per operation, not 3x.)
    **What was fixed:** eliminating the second traversal isn't possible
    without changing the `on_progress` callback contract (it expects a
    percentage, which requires knowing the total upfront) and both UIs'
    progress rendering — out of proportion for this pass. Established
    tools with the same percent-based-progress requirement (e.g. `rsync
    --info=progress2`) accept the same 2x-traversal tradeoff. What *was*
    fixed: `compute_total_size` previously ran with zero caller feedback
    and, in the GUI, zero main-loop pumping (`gui_fileop_on_progress` is
    the only place that pumps `g_main_context_iteration`) - so a large
    tree (e.g. a repo with `node_modules`) produced a silent stall in the
    TUI and a genuinely *frozen, unresponsive window* in the GUI before
    any visible progress. `compute_total_size` now takes `cb` and reports
    a "Calculating size..." progress update once per directory entered
    (not per file, to avoid flooding the callback) - the pre-pass is now
    visible and the GUI stays responsive during it. The underlying 2x
    directory-traversal syscall volume is unchanged and would need a
    progress-model change to address.

16. **[optimization/architecture] Full-screen clear plus fully independent
    terminal-size query and flush on every single draw call, on every
    keystroke** — `src/main.c:87` (`screen_clear()` — `\x1b[2J\x1b[H`,
    unconditional before every repaint, even for a single cursor move) and
    `src/screen.c` (`screen_draw_border`/`_menu_bar`/`_function_bar`/
    `_command_line` each independently call `screen_get_size()` — an
    `ioctl` — and `fflush(stdout)`). One logical redraw
    (`main.c:64-92`, fired from ~15 call sites) does 4-6 `ioctl` calls, 4-6
    separate small writes, and a full-screen clear-then-repaint flicker
    (especially visible over SSH/tmux/mosh) for changes as small as moving
    the file cursor one row. `screen.h` has no "begin/end frame" or
    dirty-region batching facility, so every caller is structurally pushed
    toward this pattern.

17. **[architecture] Duplicated "4 lines of chrome" magic number with no
    shared constant** — `src/main.c:58-62` (`panel_visible_rows()`:
    `panel_height - 4`) vs. `src/panel.c:149-152` (`panel_draw()`'s own
    `height - 4`). Both currently agree, but nothing enforces it: selection
    scroll math is driven by `main.c`'s copy, rendering by `panel.c`'s
    independent copy. A future layout change (e.g. an added status line)
    updating one and not the other desyncs the highlighted row from what's
    actually rendered.

18. **[architecture] F5 (copy) has no same-source/destination-directory
    guard, unlike F6 (move)** — `src/main.c:347-377` vs. `:378-428`. F6
    explicitly detects `active_panel->path == other_panel->path` and pops a
    Rename dialog instead of moving; F5 has no equivalent and just calls
    `fileops_copy(src, other_panel->path, ...)`. `fileops.c` does catch
    `src == dest` defensively (no data loss), but the user gets a generic
    file-op error dialog instead of the sensible in-place action the
    sibling F6 path already knows how to offer.

19. **[architecture] `main()` is a ~360-line function mixing key dispatch,
    redraw, file operations, and config mutation inline** —
    `src/main.c:254-614`. All logic lives as internal `static` helpers with
    no dispatch table or per-key handler separation; none of it is
    unit-testable in isolation. Every new keybinding grows this one
    function further.

20. **[bug] Escape-sequence collection is purely non-blocking with no
    retry/timeout, and can misread a slow-arriving multi-byte sequence as a
    bare Escape** — `src/input.c:143-170`. After the leading ESC byte, reads
    switch to `O_NONBLOCK` with no short poll/retry (no `ESCDELAY`-style
    wait). Over SSH/tmux or under load, an F-key/arrow CSI sequence's bytes
    can legitimately arrive a few ms apart; a `read()` that comes back empty
    before the sequence completes misreports the whole keypress as
    `KEY_ESC`/`KEY_UNKNOWN`.

21. **[bug] Backspace in the rename/mkdir text prompt deletes one byte, not
    one UTF-8 codepoint** — `src/screen.c:968-971`: `edited[--len] = '\0'`.
    Deleting the last character of a name ending in an accented/umlaut
    letter (e.g. renaming to "café") leaves a dangling continuation byte,
    which `utf8_visual_width`/`print_utf8_padded` then miscount — a
    one-column cursor/width glitch until a second backspace is pressed.

22. **[FIXED] [bug/architecture] `screen_draw_function_bar` has no width clamp at
    all** — `src/screen.c:258-286`. Unlike every other popup/status drawer,
    dynamic key/label content is never clipped to `cols - 2`; `remaining` is
    only used to pad, never to cut. Long/many function-key labels (future
    localization or config change) would overflow past the row and wrap
    onto adjacent rows. (Fixed together with item 8.)

23. **[architecture/style] Four near-duplicate "bordered popup frame"
    implementations** — `screen_draw_popup` (`screen.c:341-361`),
    `draw_popup_frame` (`:541-560`, the one actually meant to be shared, but
    only used by `screen_prompt_text`), `screen_prompt_buttons`
    (`:839-866`), `screen_draw_progress_popup` (`:697-723`) all hand-roll
    the identical top-border/content-lines/bottom-border sequence instead
    of sharing `draw_popup_frame`.

24. **[bug] GUI: function-bar buttons aren't gated by `g_modal_depth`,
    unlike the equivalent keyboard path** — `src_gui/gui_main.c:1015-1030`
    vs. the keyboard path's explicit guard at `:1167-1169`. Mouse-click
    correctness during a dialog/progress view currently depends entirely on
    `AdwDialog`'s own input grab, with no code-level backstop — notably F10
    (`:1020`) calls `g_application_quit()` directly, which would forcibly
    tear down the app mid-copy/move/delete if that assumption is ever wrong.

25. **[architecture/optimization] GUI: `panel_load()` always fully tears
    down and rebuilds the list model** — `src_gui/gui_main.c:126-153`,
    called unconditionally from every action (`:894-1013`) and every
    shell-bar command regardless of whether it touched the filesystem
    (`:506-533`). Every navigation/rename/delete/mkdir/shell-command clears
    and rebuilds every `TfmFileItem` from a fresh `dir_list()` (fresh
    `opendir`+`readdir`+`stat` per entry): (a) `GtkSingleSelection`'s
    position resets every time, losing the user's cursor/scroll position
    after every single-item change, not just directory changes; (b) it's
    O(n) real syscalls even for a shell command that changed nothing. The
    doc comment at `:502-505` ("reload only if the command changed files")
    doesn't match the unconditional-reload implementation.

26. **[bug] GUI: `colors.toml` inline comments defeat the unquote parser,
    and the result is spliced into CSS unvalidated** —
    `src_gui/omarchy_theme.c:12-29` (`trim_and_unquote`). A legal line like
    `accent = "#f38d70" # my favorite` has trailing non-whitespace after the
    closing quote, so the "ends with a quote" check fails and the *whole*
    string (leading `"` + comment included) is kept, then silently
    truncated to fit the 16-byte field. This garbled value is spliced
    verbatim into `@define-color accent_color <value>;` with no syntax
    validation and no `"parsing-error"` handler on any of the three
    `GtkCssProvider`s in the file — a malformed theme value silently
    degrades theming with zero visible error.

27. **[optimization] GUI: `GtkCssProvider` is destroyed and recreated on
    every call instead of reloaded in place** —
    `src_gui/gui_main.c:1132-1154` (`apply_font_size`, on every Ctrl+±/0
    keypress — an auto-repeat-held key churns this repeatedly) and
    `:213-307` (`apply_omarchy_theme`, on every theme-reload signal).
    `gtk_css_provider_load_from_string()` can be called again on the same
    provider instance; there's no need for the
    remove-from-display/unref/recreate/re-add dance each time.

### Low

28. **[bug/style] `dir.c` bypasses the project's own `path_join()` helper**
    — `src/dir.c:79-82` uses raw `snprintf` with no truncation check, even
    though `dir.c` already includes `tfm_common.h`. A path exceeding
    `PATH_MAX` silently truncates and `stat()` runs against the wrong path.
    Inconsistent with `fileops.c`'s established pattern.

29. **[bug/style] Inconsistent `$HOME`-unset fallback** —
    `src/config.c:23-26` (`config_get_dir()`) falls back to `"."` (CWD,
    varies by launch location) vs. `:39-42` (`config_set_defaults()`)
    falling back to `"/"`. If `$HOME` is unset, the config file's location
    becomes launch-directory-dependent while default panel paths are fixed.

30. **[architecture] `FileOpChoice` enum is shared between two semantically
    different questions** (retry-choice for `on_error` vs. overwrite-choice
    for `on_overwrite`) — `include/fileops.h:10-15`. Nothing at the type
    level stops `on_error` returning `OVERWRITE` or vice versa; handled
    defensively at each call site today (Pass 1's fix), but two distinct
    enums would make the invalid states unrepresentable.

31. **[architecture/doc] `fileops.h`'s doc comment for `fileops_move`**
    doesn't mention that choosing Overwrite on a type-mismatched
    destination (file-over-dir or vice versa) triggers a full recursive
    delete of the entire destination tree first (`fileops.c:431-438`) —
    understates how destructive that choice is for directories.

32. **[architecture] `dir_list()`'s failure contract is underspecified** —
    `include/dir.h:13` — collapses opendir/malloc/realloc failures into a
    single `-1` with no errno/reason, and (per item 4 above) doesn't
    guarantee `*out_entries`/`*out_count` are set to a defined value on
    failure.

33. **[style] `config.c` builds paths via raw `snprintf`, not the shared
    `path_join()`** — `src/config.c:27,34`. An extremely long `$HOME` would
    silently truncate the resulting config path.

34. **[bug] `config_load()`'s per-field `snprintf` calls silently truncate**
    any `tfm.ini` value longer than its destination field (e.g. a 40-char
    string into `border_color[32]`) with no error/log —
    `src/config.c:116-136`. Low impact (small enum-like fields) but a
    silent-truncation pattern.

35. **[architecture] `config_save()` returns `void`** —
    `include/config.h:37-39` — giving the caller no way to learn saving
    failed (disk full, permission error); the only symptom is silently
    losing the previous session's panel paths on next launch.

36. **[bug] `compute_total_size()` silently drops an entry from the total
    if `path_join` fails for it** — `src/fileops.c:80-83` — unlike the
    analogous case in `copy_recursive`/`delete_recursive`, which at least
    calls `report_error`. Only cosmetically undercounts the progress
    denominator, but it's a silently-swallowed failure path.

37. **[bug/consistency] `builtin_cd()`'s path construction has no
    truncation check**, unlike the project's `path_join()`-based pattern
    elsewhere — `src/main.c:135-143`. A truncated-but-valid near-`PATH_MAX`
    path could `realpath()`-resolve successfully to the *wrong* directory
    with zero warning. Backs both the typed `cd` command and every
    Enter-to-navigate action.

38. **[bug] `input_enable_raw_mode()`/`input_disable_raw_mode()` never
    check `tcgetattr()`/`tcsetattr()` return values** —
    `src/input.c:48-79`. If stdin isn't a tty or the ioctl fails,
    `g_raw_mode_active` is still set to `1` regardless of whether the mode
    change actually took effect, with no diagnostic.

39. **[optimization] `compute_layout()` is invoked twice per arrow-key
    press** — once directly for `panel_visible_rows()`, once again inside
    the immediately-following `redraw_ui()` (`src/main.c:481-490` vs.
    `:64-68`) — each triggering its own `ioctl(TIOCGWINSZ)`.

40. **[bug] `compute_layout()` has no minimum-terminal-size clamp** —
    `src/main.c:39-56` — for a very small terminal, `inner_width`/panel
    widths can go negative and are passed straight into
    `screen_draw_box()` unguarded.

41. **[optimization] `input_read_key()` treats real EOF and an
    `EINTR`-interrupted read identically**, adding ~20ms latency to every
    terminal-resize redraw — `src/input.c:126-135` — both take the same
    "sleep 20ms, return `KEY_NONE`" path.

42. **[style/doc] `input_read_key()`'s doc comment doesn't disclose that it
    can spuriously return `KEY_NONE`** with no key pressed —
    `include/input.h:39-40` says it "blocks until a key is pressed."

43. **[style] Duplicated "get the selected, non-`..` entry" guard across
    F5/F6/F8** — `src/main.c:350-354,381-383,452-454` — could be one
    helper (e.g. `get_selectable_entry(panel)`).

44. **[architecture/style] `panel.c`'s `icon_for_entry()` hardcodes
    narrow, project-specific heuristics** (folder names `include`/`src`/
    `build`, specific extensions) inside an otherwise generic panel
    renderer — `src/panel.c:38-78` — no handling for equally common names
    (`docs`, `tests`, `.git`, `node_modules`) and no way to extend the
    mapping.

45. **[style] Magic buffer size `300` duplicated three times with no
    shared constant** — `src/main.c` `confirm_msg[300]` (`:455`),
    `synthetic_cmd[300]` (`:201`), `full_msg[300]` (`:327`).

46. **[bug/low] `item_display` in the progress popup is pre-truncated on
    the wrong end before the "keep visible tail" logic runs** —
    `src/screen.c:651-672`. The initial `snprintf` into a 256-byte buffer
    keeps only the *first* 255 bytes of a path that can be up to
    `PATH_MAX`; the later logic is specifically designed to keep the tail
    and prefix "...", but for paths >255 bytes that tail is already gone,
    and the cut can split a multi-byte UTF-8 sequence.

47. **[style] `screen_draw_progress_popup` measures `title` with
    `strlen()` instead of `utf8_visual_width()`**, inconsistent with the
    rest of the same function — `src/screen.c:640`. Harmless while titles
    are static ASCII.

48. **[bug/low] `screen_draw_border`'s title truncation is byte-based**
    (`strlen`/`%.*s`), same class as Pass 1 item 11 — `src/screen.c:470-480`
    — not currently triggered (only call site passes a static ASCII title)
    but the public API is unsafe for any future non-ASCII title.

49. **[architecture] `screen_prompt_text`'s internal edit buffer is
    hard-coded to 256 bytes, ignoring the caller-supplied `buffer_size`** —
    `src/screen.c:930`. Both current call sites happen to also use 256-byte
    buffers, so nothing observable today, but reusing this function for a
    longer field would silently cap input at 255 chars.

50. **[style/architecture] `COLOR_TABLE`/`BG_COLOR_TABLE` are hand-maintained
    parallel arrays** with no single source of truth per color name —
    `src/screen.c:35-71` — `screen_color_name_is_valid()` only checks
    `COLOR_TABLE`, so it can validate a name `bg_color_lookup()` doesn't
    recognize.

51. **[architecture] `screen.c` mixes stateless drawing with blocking modal
    control flow** — `screen_prompt_buttons`/`screen_prompt_text`
    (`:870-893,949-985`) call `input_read_key()`/
    `input_consume_resize_flag()` directly inside their own redraw loop,
    coupling rendering to `input.c`'s blocking I/O and making dialog
    behavior impossible to drive/test without a live terminal.

52. **[bug/low] `editor.c`'s single-quote-escaping loop truncates the path
    silently** if it hits its bound (`qi < sizeof(quoted)-5`) —
    `src/editor.c:34-45` — pathological (needs hundreds of literal `'` in a
    near-`PATH_MAX` path) but unlike the command-length check right after
    it (`:48`, which fails explicitly), a truncated path here has no guard
    and would silently open the wrong file.

53. **[style] `shell.c`'s forked child doesn't close/`O_CLOEXEC` inherited
    file descriptors beyond the standard streams** — `src/shell.c:23-30` —
    currently harmless (every other `open`/`fopen` call site in the
    codebase is short-lived) but no structural guard against a future
    long-lived fd leaking into every shell command/`$EDITOR` invocation.

54. **[optimization] `splash.c`'s `draw_plain_row` mallocs/frees a
    per-frame buffer** instead of once outside the animation loop —
    `src/splash.c:133-153` — harmless (one-time startup animation) but
    avoidable.

55. **[optimization] `splash.c`'s `draw_big_row` emits a full SGR
    color-set+reset sequence per lit pixel** instead of once per contiguous
    same-color run — `src/splash.c:124` — bounded by the small pixel count,
    just avoidable bytes.

56. **[bug/low] `splash.c`'s subtitle row isn't clamped against terminal
    `rows`** — `:169` — on a <~7 row terminal the subtitle can be
    positioned past the last row (terminals generally clip this
    harmlessly).

57. **[style] Duplicated "other panel" lookup in the GUI, unlike `main.c`'s
    extracted `other_panel_of()`** — `src_gui/gui_main.c:897,920,964,988` —
    `GuiPanel *other = &g_panel[g_focused_panel == 0 ? 1 : 0];` repeated
    identically in `action_copy`/`action_move`/`action_mkdir`/
    `action_delete`. Pass 1 extracted the exact same pattern in `main.c`;
    the GUI never got the equivalent helper.

58. **[style] Magic numbers without named constants in the GUI** —
    `src_gui/gui_main.c:1090,1100,1121,1124-1125` (default font size `11.0`
    repeated 4×), `:1209` (minimum font size `5.0`, with no corresponding
    maximum clamp on Ctrl+Plus growth), `:205` (luma threshold `150.0` in
    `contrasting_fg_for`, no comment on why 150).

59. **[style/architecture] GTK factory setup/bind coupled only by implicit
    widget-tree position, no defensive null checks** —
    `src_gui/gui_main.c:325-354` — `on_factory_bind` locates the icon/label
    via `gtk_widget_get_first_child`/`_get_next_sibling` with no
    `g_object_set_data`-based lookup and no NULL check before casting. Any
    future change to `on_factory_setup`'s child order silently breaks
    `bind` instead of failing loudly.

60. **[style] Space bar is silently swallowed when a panel has focus in the
    GUI** — `src_gui/gui_main.c:1231` — `g_unichar_isgraph(' ')` is false,
    so unlike every other printable character it isn't forwarded to the
    shell entry, and `GtkListView` has no bound action for it either. Minor
    UX inconsistency; no multi-select feature exists to bind it to anyway.

### Cross-cutting architecture/style themes

- **Recurring bug classes at unreached call sites**: three Pass-1-fixed bug
  classes (signed-char UTF-8 gate, stale-resize-flag consumption, byte-vs-
  codepoint truncation) reappeared in Pass 2 at call sites the first pass
  didn't cover (items 5, 6, 21, 48). Suggests these are better fixed once
  at the type/helper level (`KeyEvent.ch`'s type; a single
  `input_read_key_and_drain_resize()` wrapper used everywhere raw mode is
  re-entered) than patched per call site again.
- **TUI/GUI duplication without a shared layer**: `other_panel_of()`
  (item 57), the "4 lines of chrome" constant (item 17), and the bordered-
  popup-frame pattern (item 23) are each solved once in one front-end and
  re-solved (or not) independently in the other. There's no shared
  "UI helpers" layer above the two front-ends' core-module usage.
- **No regression-test harness**: every recurrence above would have been
  caught by even a minimal automated test over `input_read_key`/UTF-8
  handling; all verification so far has been manual (`/tmp` repros per
  [[feedback_tfm_workflow]]). Worth considering for `fileops.c` and the
  UTF-8 helpers specifically, given how many findings cluster there.

### Suggested fix order (Pass 2)

1. Same-inode aliasing data-loss bug (item 1) — critical.
2. `dir_list()` failure contract + `panel_reload`/`panel_init` guard (item 4).
3. Copy-into-own-descendant guard + permission preservation (items 2, 3).
4. GUI close-request handling during an active file-op (item 10).
5. Non-ASCII command-line input + stale-resize-flag recurrences (items 5, 6).
6. `shell.c` EINTR/pump-loop fix (item 9) + terminal-restore signal handlers (item 7).
7. `screen.c` bar-clipping fix (item 8).
8. Remaining medium-severity items, roughly in the order listed above.

## Pass 1 (fixed — historical)

## Critical (data loss)

1. **[FIXED]** **`fileops_move` destroys the whole destination directory instead of merging** —
   `src/fileops.c:375-386`. Moving `dirA/` onto an existing `dirB/` calls
   `delete_recursive(dest, cb)` on all of `dirB` first, wiping any files unique
   to `dirB` before the move happens. Contradicts `include/fileops.h:42`'s doc
   claiming move's overwrite handling is "like `fileops_copy`" (which actually
   merges).

## Critical (GUI keyboard hijack / reentrancy)

2. **[FIXED]** **`on_window_key_pressed` captures every keypress, including inside modal
   dialogs** — `src_gui/gui_main.c:1003-1083`. Installed as a
   `GTK_PHASE_CAPTURE` controller on the top-level window with no "dialog/op
   active" guard:
   - Rename/New Folder dialogs are effectively unusable — typed characters get
     redirected into the shell entry instead of the dialog's `GtkEntry`, and a
     following Enter can execute the stray text as an arbitrary shell command
     in the active directory.
   - Tab can't reach an open alert dialog's buttons.
   - F5–F8 aren't gated on "operation already running" — a stray keypress
     during a long copy can re-enter `action_copy`/`action_move`/`action_delete`,
     tearing down the still-running operation's progress dialog.
   - F10 quits unconditionally, even from inside a nested `g_main_loop_run`
     (dialog/prompt), risking a hang or use-after-free on destroyed widgets.
   - Fix direction: gate Tab/F-key/char-redirect branches on an explicit
     "no modal dialog and no file-op in progress" flag, or check that the
     current focus widget is a descendant of the panel views / shell entry
     before acting.

## High severity

3. **[FIXED]** **Unbounded GObject leak on every directory load** — `src_gui/gui_main.c:111`.
   `tfm_file_item_new()` returns an owned ref, `g_list_store_append()` takes
   its own additional ref, nothing unrefs the first — every visible row (plus
   its heap `name`) leaks permanently on every navigation/reload/shell-command.
   Fix: `g_object_unref(item)` immediately after `g_list_store_append()`.

4. **[FIXED]** **Symlinked directories can't be entered** — `src/dir.c:72-84`. Only
   `DT_DIR` gets `is_dir=1`; `DT_LNK` falls into the final `else` and gets
   `is_dir=0`, so `main.c:186` (`enter_selected_entry`) refuses to descend
   into a symlinked directory with Enter. Fix: treat `DT_LNK` like
   `DT_UNKNOWN` (stat-follow it).

5. **[FIXED]** **`config_save()` can silently corrupt/truncate `tfm.ini` on write failure**
   — `src/config.c:125-151`. `fopen(path, "w")` directly on the live file, no
   temp-file+rename, no `fprintf`/`fopen` error checking. Fix: write to a temp
   file in the same dir and `rename()` over the target only on success.

6. **[FIXED]** **Busy-loop / 100% CPU if stdin is EOF or closed** — `src/input.c:106-113`
   + `main.c:327-333`. `input_read_key()` returns `KEY_NONE` immediately on
   EOF/error instead of blocking; the main loop has no idle/sleep path.

7. **[FIXED]** **`action_delete()` never calls `gui_progress_hide()`** —
   `src_gui/gui_main.c:839-864`. Harmless today only because
   `delete_recursive()` doesn't yet report progress; the moment it does, the
   non-closable progress dialog (`adw_dialog_set_can_close(dialog, FALSE)`)
   would hang the GUI forever. Fix: add `gui_progress_hide();` after the
   `fileops_delete()` call, matching copy/move.

8. **[FIXED]** **Untrusted filenames fed as Pango markup into `AdwAlertDialog` bodies** —
   `gui_fileop_on_overwrite` (`gui_main.c:648-651`), delete-confirm
   (`gui_main.c:848-850`), `gui_fileop_on_error` (`gui_main.c:619-643`). A
   filename containing `&`/`<`/`>` breaks markup parsing right when the
   dialog needs to clearly show what's about to be deleted/overwritten. Fix:
   `adw_alert_dialog_set_body_use_markup(dialog, FALSE)` or
   `g_markup_escape_text()` the dynamic content.

9. **[FIXED]** **Cross-device move cleanup silently swallows all errors** —
   `src/fileops.c:265-292` (`remove_recursive`, used at line 407). `opendir`
   failures are treated as "no children"; `unlink`/`rmdir` return values are
   discarded entirely, no callback reporting. After an EXDEV
   copy+delete-source fallback, a partial cleanup failure leaves the user
   with duplicate data at both source and destination and zero indication
   anything went wrong.

10. **[FIXED]** **Several error paths don't actually implement a retry loop** — "Retry"
    silently degrades to "Skip" in some fileops.c call sites (pattern is
    correct in `copy_file`/`copy_recursive`/`delete_recursive`, inconsistent
    elsewhere).

11. **[FIXED]** **UTF-8 truncation splits multi-byte characters in the progress popup** —
    `src/screen.c:640-658,691,694`. Byte-based `strlen`/`%.*s` instead of
    `utf8_visual_width()`/`print_utf8_padded()` (used by every other popup in
    the file) — an accented/umlaut filename long enough to truncate can
    render mojibake or misaligned.

12. **[FIXED]** **Non-ASCII keystrokes silently rejected in rename/mkdir text input** —
    `src/screen.c:945`. `key.ch >= 32 && key.ch < 127` — `KeyEvent.ch` is a
    signed `char`, so UTF-8 bytes ≥0x80 become negative and always fail the
    gate. A user can't type `ü`/`ö`/`é` etc. when renaming, even though the
    project has UTF-8 filename test cases elsewhere.

13. **[FIXED]** **Stale SIGWINCH resize flag can eat the next real keystroke** — many
    `input_read_key()` call sites in `main.c`/`screen.c` (popups, prompts)
    never call `input_consume_resize_flag()`. Resizing the terminal while a
    dialog is open leaves the flag set; the next real keystroke gets
    discarded as a "just a resize, redraw and continue" event.

14. **[FIXED]** **Overwrite-choice handling fails open, not closed** —
    `fileops.c` `copy_file`/`fileops_move`. Only `ABORT`/`SKIP` are checked
    explicitly; any other enum value (e.g. an accidental `RETRY` from a
    buggy UI callback, since the enum is shared with `on_error`) falls
    through to "proceed as overwrite" — silently overwriting without
    consent.

## Medium severity

15. **[FIXED]** **GUI never persists config** — `gui_main.c` calls `config_load()` but
    never `config_save()`, unlike `main.c:616`. Closing/reopening `tfm-gui`
    forgets last-visited directories, unlike the terminal UI.

16. **[FIXED]** **GUI swallows editor-launch failures** that the terminal UI reports —
    `gui_main.c` `on_item_activated` vs `main.c:513-522`. `$EDITOR`
    unset/missing gives GUI users zero feedback.

17. **[FIXED]** **Shell bar has no `cd` builtin and swallows exit codes** —
    `gui_main.c:404-414` vs `main.c:551-576`. `cd ..` in the GUI shell bar
    silently does nothing; any failing command fails silently.

18. **[FIXED]** **`editor_open()` blocks the whole GTK main loop with no event pumping** —
    `gui_main.c:337-346`. Unlike the copy/move progress path, nothing pumps
    `g_main_context_iteration` while the editor runs — the window can appear
    frozen/"not responding".

19. **[FIXED]** **F5 Copy doesn't reload the source panel** when both panels show the
    same directory — `main.c:339-365` (F6/F7/F8 all guard this with
    `strcmp(active_panel->path, other_panel->path)`, F5 doesn't). The
    identical bug exists independently in the GUI's `action_copy()`
    (`gui_main.c:768-769`).

20. **[FIXED]** **`config.c`'s line buffer too small for path fields** — `config.c:72`.
    `char line[512]` but `left_path`/`right_path` are `PATH_MAX` (4096) —
    a `tfm.ini` value longer than ~510 chars truncates and the leftover tail
    is misread as a bogus separate line.

## Minor / doc mismatches / dead code

- **[FIXED]** `README.md` was completely stale — described the bare project
  skeleton, no mention of `src_gui/`, GTK GUI, or any implemented feature
  (copy/move/delete/mkdir, theming, shell integration). Rewritten in
  English to reflect actual features, both binaries, install targets, and
  structure.
- **[FIXED]** `join_path` (`main.c:97-101`) and `gui_join_path` (`gui_main.c:457-461`)
  are byte-identical duplicated logic. Extracted to `path_join()` in new
  `include/tfm_common.h` / `src/tfm_common.c`.
- **[FIXED]** `PATH_MAX` fallback macro duplicated verbatim across 6 files
  (`dir.c`, `editor.c`, `fileops.c`, `omarchy_theme.c`, `config.h`, `panel.h`).
  Centralized in `include/tfm_common.h`.
- **[FIXED]** `other_panel` lookup duplicated 4× in `main.c` (F5/F6/F7/F8).
  Extracted to `other_panel_of()`.
- **[FIXED]** "Path too long" / hide-draw-read-show error-popup block
  duplicated ~7× in `main.c`. Extracted to `tui_show_popup()`.
- **[FIXED]** Dead `"."`-entry guard checked 4× in `main.c` — `dir_list()`
  already filters `.` before it reaches a Panel. Removed.
- **[FIXED]** Four near-identical `AdwAlertDialog` construction blocks in
  `gui_main.c` (`show_error_dialog`, `confirm_dialog`, `gui_fileop_on_error`,
  `gui_fileop_on_overwrite`) — collapsed into one parameterized
  `show_alert_dialog()` helper.
- **[FIXED]** `OmarchyThemeColors.muted` field is parsed but never read
  anywhere. Removed the field and its parsing.
- **[FIXED]** `omarchy_theme_load()` required both `mode` and `accent` to
  report success, so a theme file missing just `mode` skipped theming
  entirely even though other fields parsed fine. Now only `accent` is
  required; missing `mode` defaults to light.
- **[FIXED]** Theme/config line parsers use a 256-byte buffer; a line >255
  chars gets silently split by `fgets` (latent, not currently triggered).
  Bumped `omarchy_theme.c`'s parsers to 1024 bytes.
- **[FIXED]** `splash.h` documented a scroll-out exit animation that isn't
  implemented (`splash.c:210-222` hard-cuts instead). Doc updated to match
  actual behavior.
- **[FIXED]** `config.h`'s doc for `config_set_defaults()` said it defaults
  to "current directory"; it actually defaults to `$HOME`. Doc corrected.
- **[FIXED]** `read_terminal_font_size()`'s `"font"` prefix match
  (`gui_main.c:958`) was looser than intended (`strncmp(p, "font", 4)`) —
  now matches `"font="` specifically.
- **[FIXED]** `fileops.c`'s `snprintf(child, ...)` call sites never checked
  for truncation, unlike `main.c`'s `join_path()`. All sites now use the
  shared `path_join()` and report/skip on truncation instead of silently
  operating on a wrong path.
- **[FIXED]** `copy_file` used `stat()` (follows symlinks) to detect an
  existing destination while `copy_recursive` used `lstat()` everywhere
  else — inconsistent handling of dangling symlinks at the destination
  path. `copy_file` now uses `lstat()` too.

## Suggested fix order

1. `fileops_move` destination-merge data-loss bug.
2. GUI global key-handler / dialog-interaction hazard.
3. GUI `TfmFileItem` reference leak.
4. Remaining high-severity items, roughly in the order listed above.
