# Plan: centered activity box in IGC

Status: implemented 2026-10-13 (uncommitted at the time of writing). Verified in
MAME: multi-file C:->C: copy (box rows 8-12 with bar and `ESC cancels copy`,
row 23 blank, panels intact after), overwrite prompt over the box (box and bar
intact after Y and A), ALT+ESC copy cancel (no remnants), mkdir afterwards,
directory-tree delete (hint only during `fops_delete_dir`, box shrinks to 4 rows
for plain files with the freed row restored), a single directory delete after a
cancelled copy (clean box, no leftover hint), `Reading directory...` 3-row box,
editor F2 `File saved` on row 23. Not run: serial tests (growth, mixed serial
batch, FIFO timing), disk-image ESC abort, write-protect error, real hardware.
Deviations: the bar row is the constant `ACT_BAR_Y` (row 10), not a `g_bar_y`
variable, since it is always box row 2; `act_layout` keys on height only, and
the slow path always resets the 26-cell title area of the top border, so a title
appearing or vanishing needs no full layout.

## Goal

Show what IGC is busy with (copying, deleting, reading a directory or the
server, connecting, checking or writing a disk image) in a boxed window in the
middle of the screen, where the eye already is, instead of on row 23 beside the
F-key bar. The box grows from one content line to a few: a message, an optional
progress bar, and an optional hint such as `ESC aborts`. Errors and one-off
notices stay on row 23. Almost no call site changes. The per-packet progress
tick keeps the same maximum number of VRAM writes as today.

## Facts the design rests on

**Screen layout** (`igc.h`). Panels use rows 0-21, row 22 is blank,
`ROW_STATUS` 23, `ROW_FKEYS` 24. The whole UI is drawn in `ATTR_DIM`; dialogs
use a dim border with an `ATTR_DIM_REV` title. Plain `ATTR_NORMAL` (bright) is
unused, so a bright box stands out at no cost.

**Today's API** (`ui.c` ~350-495).
- `ui_status`, `ui_error` and `ui_clear_status` write row 23.
- `ui_show_progress(title, file, cur, total)` fills all 80 cells of row 23, then
  writes `Title: file [20-cell bar] NN%` and records `g_prog_bar_len` /
  `g_prog_pct`. The fill also wipes a previous `ui_error` (ui.c:406).
- `ui_progress_tick` writes at most the flipped bar cells plus 4 percent
  characters, and returns early when the percent is unchanged. It runs between
  serial packets (`sfs_progress_cb`, `diskimg.c run_tracks`). The Victor's
  polled receive has a 3-byte FIFO at 38400, so the tick must not grow.
- Serial transfers paint their frame before the request goes out
  (`sfs_xfer_begin`, serialfs.c:285), outside the packet gap.

**How callers use it.**
- Every `ui_status` outside the editor is bracketed by `ui_clear_status` on
  every exit path (`panel_read_dir` has a single exit; `serialfs_read_dir`,
  `loc_preflight` and `main.c` clear before each return).
- Every progress batch ends in `ui_hide_progress`, except
  `serialfs_get_file` / `put_file`, which rely on their caller (`main.c`
  `Fetching from server...`, or the `fileops.c` batch).
- Nearly every `ui_error` is followed by `kbd_wait()`. The exceptions are
  serialfs.c:193/224 and the editor, where the error is left visible.
- Dialogs opened during an activity close before it continues: `dlg_overwrite`,
  `dlg_delete_confirm`, Retry/Abort in `disk_track`, `dlg_alert` in
  `run_tracks`. So stacking is last opened, first closed (LIFO).

**Where ESC is really polled.**
- Local file copy: after each buffer written (fileops.c:236). A cancel stops
  the whole batch (`result != FOPS_CANCEL` loops).
- Recursive local directory delete: after each child (fileops.c:466).
- Never polled: `fops_delete_file` (fileops.c:374), the top level of the delete
  batch, renames in a move (fileops.c:913; a same-drive move whose rename fails falls back to the polled copy), single serial file transfers, and
  the disk-image preflight.
- Serial trees: only between children, after whole-file transfers
  (serialfs.c:704/783/858). A cancel returns -1. The outer batch then reports
  `Serial transfer failed` (fileops.c:598) and carries on with the next
  selected item.
- Disk images: between completed tracks, not after the last (diskimg.c:550).

**Other pieces.**
- `dlg_open` mallocs a `w*h*2` save buffer and draws a fixed-size box.
  `dlg_close` restores, frees and calls `mouse_new_context()`.
- Only `kbd_wait()` shows the mouse pointer, and it hides it again before
  returning, so the box needs no mouse handling.
- The normal repaint is `main.c:454`: `ui_draw_panels` + `ui_draw_title_bar`.
  `ui_draw_frame` (ui.c:33) draws only borders and the bottom rows.
- After `editor_edit` returns, the file-manager screen has already been restored
  (editor.c:1057).
- The copy buffer is only 256-8192 bytes (`COPY_BUF_*`), allocated on demand.
- DGROUP is at 0x4BA0 (19 KB) of 64 KB.

## Design

**Scope.**
- (a) Bracketed activity goes in the box: `ui_status`...`ui_clear_status` and
  `ui_show_progress`...`ui_hide_progress`.
- (b) Fire-and-forget notices go to row 23 through a new `ui_notice()` (the
  old `ui_status` body). Today that is only the editor's `File saved`; a box
  there would never be closed.
- (c) `ui_error` is unchanged, on row 23 in reverse video.

Rejected: turning `ui_error`+`kbd_wait` into `dlg_alert` (about 50 sites, and
serialfs.c:193/224 deliberately do not block). Also rejected: drawing errors
inside an open box, which puts errors in two places depending on state.

**Geometry.** Fixed width `ACT_W` = 56 at `ACT_X` = 12, fixed top `ACT_Y` = 8,
growing downward, maximum height `ACT_MAXH` = 6.

| Row in box | Content | Present when |
|------------|---------|--------------|
| 0 | top border; title centered, `ATTR_REVERSE` | always (blank title for `ui_status`) |
| 1 | line 0: message or file name | always |
| 2 | `[20-cell bar] NNN%`, centered | progress active |
| next | extra lines 1 and 2, packed (empty ones skipped) | if set |
| last | bottom border | always |

Height = 3 + bar + number of non-empty extra lines:
- A status message is 3 rows (8-10).
- Progress is 4 rows (8-11).
- Progress plus one hint is 5 rows (8-12).
- Progress plus two extra lines is 6 rows (8-13).

Border and text use `ATTR_NORMAL`. The frame is drawn only when the layout
changes, never per tick.

**Growth: option (iii).** On open, save the full 56 x 6 rect once and draw a box
of the current height. When the height changes, redraw the frame. When it
shrinks, first restore the freed rows from the save buffer. Rows are contiguous,
so this is `scr_restore_rect(x, y+h, w, MAXH-h, save + h*w)`. Close restores the
current height; rows freed by an earlier shrink are already back.

Rejected options:
- (i) Always drawing the maximum height makes a `Reading directory...` box twice
  the size it needs.
- (ii) Close and reopen on change flickers, and would save the old box as
  background.

**Save buffer: static.** `static uint16_t g_act_save[ACT_W*ACT_MAXH]` takes
672 bytes of `_BSS` and passes straight to the far `scr_save_rect` in the
compact model. It cannot fail, so there is no row-23 fallback path. Rejected:
`mem_alloc` per open, which needs a second rendering path for an allocation that
can fail on `MEM_TINY`.

**State and limits** (statics in `ui.c`; every limit counts the NUL):
- `g_act_open`, `g_act_bar` (progress active), `g_act_h`.
- `g_act_title[ACT_TITLE_CAP]`, cap 24, so 23 characters.
- `g_act_line[3][ACT_LINE_CAP]`, cap 53, so 52 characters. Line 0 is the
  message or file name; lines 1 and 2 are extras.
- The existing `g_prog_bar_len` / `g_prog_pct`, plus `g_bar_y` (the bar's
  screen row).

Text is copied with `str_copy_n(dst, src, CAP)`, which takes the buffer
capacity (util.c:31). Longer text is truncated. Today's titles are under 12
characters, and file names are 8.3 or capped at 30.

**Painting rules.**
1. `act_layout()` runs only when the height or the title's presence changes.
   It restores freed rows, draws the frame, then all lines.
2. Otherwise a slow-path update repaints only the title text, line 0 and the
   bar. That is about 80-100 cells, the same as today's row-23 repaint. A full
   five-row layout is about 280 cells plus text.
3. Whenever the layout or line 0 is repainted while `g_act_bar` is set, the bar
   is drawn at the cached `g_prog_bar_len` / `g_prog_pct`, never empty. Reason:
   a following tick at an unchanged percent returns early and would leave the
   bar blank.
4. `ui_status` and `ui_show_progress` (the slow paths, not the tick) still blank
   row 23, so an error from one file does not linger through later files.
5. Layout changes happen only at operation boundaries: between files, before a
   serial request is sent, between tracks. They never happen in the packet gap.

**Tick.** `ui_progress_tick` returns at once if `!g_act_bar`. Otherwise it is
unchanged: at most 20 bar cells + 4 percent characters, at `ACT_X + PROG_BAR_X`
and row `g_bar_y`. The flag test and the variable row add a few instructions,
so the real-hardware serial test below is the check. Rejected: a 40-cell bar,
which doubles the worst case in the packet gap for looks only.

**Open/close semantics.** There is one global box and no depth counter, since
callers are not strictly paired (`sfs_xfer_begin`).
- `ui_status(msg)`:
  - If closed, it opens the box with no title and no bar.
  - If open, it sets line 0, blanks the title and drops the bar.
- `ui_show_progress(...)`:
  - If closed, it opens the box.
  - If open, it sets the title, line 0 and the bar. For example,
    `Fetching from server...` grows into `Copying / name / [bar]`.
- Neither call touches extra lines. Their owner sets and clears them (see
  hints).
- `ui_clear_status()` / `ui_hide_progress()`:
  - If the box is open, they restore the current `g_act_h` rows, then reset all state
    (`g_act_open`, `g_act_bar`, `g_act_h`, title and all three lines) and call
    `mouse_new_context()`, as `dlg_close` does.
  - They always blank row 23 (`main.c:195` relies on it). With no box open,
    that blank is all they do.
- Nested pairs collapse harmlessly. If an inner close shuts the box, the outer
  close only blanks row 23, and a stray tick hits the `!g_act_bar` guard.
- `ui_error` while the box is open writes row 23 only.
- **No repaint safety net.** Correctness relies on bracketing: a grep finds no
  `ui_draw_*` or `panel_read_dir` between open and close. Rejected: calling
  `ui_clear_status` at main.c:454. It would also blank row 23 and erase errors
  that are meant to stay visible, such as serialfs.c:224. Rejected: a reset in
  `ui_draw_frame`, which is not on the repaint path and would leave the box
  un-restored.
- Rule for AGENTS.md: open and close dialogs and the box in LIFO order. No
  current path breaks this.

**Hints: set by the code that polls ESC, cleared when that operation ends.**
`ui_status_line(1, text)` stores the text and draws it if the box is open.
Otherwise it shows when the box next opens. Close clears it.
`ui_status_line(1, NULL)` removes it and shrinks the box. Because each owner
clears its own hint, a serial tree followed by a serial file in one batch does
not carry the hint over.

## API (ui.h)

```c
void ui_status(const char *msg);                  /* open/refresh box, line 0 = msg  */
void ui_status_line(uint8_t n, const char *text); /* n = 1..2; NULL/"" removes        */
void ui_clear_status(void);                       /* close + reset box, blank row 23  */
void ui_show_progress(const char *title, const char *filename,
                      uint16_t current, uint16_t total);   /* unchanged signature */
void ui_progress_tick(uint16_t current, uint16_t total);   /* unchanged signature */
void ui_hide_progress(void);                      /* = ui_clear_status                */
void ui_notice(const char *msg);                  /* NEW: old row-23 ui_status        */
void ui_error(const char *msg);                   /* unchanged, row 23                */
```

The internals are `act_layout()` and `act_bar(full)`, the old bar paint at
`g_bar_y` from the cached length. Expected size is about 110 lines replacing
about 90.

## Call-site changes

| File:line | Change |
|-----------|--------|
| editor.c:964 `ui_status("File saved")` | **`ui_notice`** |
| fileops.c local copy batch (773/792) and `fops_copy_dir` (336) | **set** `ESC cancels copy` before the batch, clear after. Every item reaches the polled data loop. |
| fileops.c `fops_move` (3 copy sites) | `move_copy()` helper **sets** `ESC cancels move` around the copy phase only: renames never poll, and the source delete that follows ignores a cancel. Same-drive moves whose rename fails fall back to copy and get the hint too. |
| fileops.c local delete batch (992/1019) | **set/clear** `ESC stops after this file` around each `fops_delete_dir` call only. Plain file deletes never poll. |
| fileops.c serial batch (557) and serial delete (636/658) | none. Hints come from the tree functions. |
| serialfs.c `serialfs_get_tree` / `put_tree` / `delete_tree` (~872/890/900) | **set** `ESC skips rest of this folder` on entry, **clear** on every return. Single-file transfers get no hint. |
| diskimg.c:627/723 | **set** `ESC aborts after this track` before `ui_show_progress`. Cleared by `ui_hide_progress`. |
| diskimg.c:421-430 preflight; panel.c:206/281; serialfs.c:197/221/244/251/422/849; main.c:143-211; diskimg.c:549/631/726; fileops.c:445/607/666/806/938/1029 | none |
| all `ui_error` sites (about 45) | none |

That is one changed call and about eight hint set/clear pairs. Everything
else compiles untouched.

## Risks / open points

- **Serial tree cancel is not a batch abort.** ESC skips the rest of that
  folder; then `Serial transfer failed` appears and the batch continues. The
  hint wording reflects this. Making it abort the selection (return a distinct
  cancel code) is a separate fix.
- **Flash on every directory change.** `panel_read_dir` opens and closes the box
  on every Enter. If that is noisy in MAME, switch panel.c:206/281 to
  `ui_notice` / `ui_clear_status` (two lines), or use the box only for floppies.
- **Close/reopen between phases** (`Checking image file...` then `Writing A:`;
  `Connecting...` then `Reading server...`) is acceptable and cosmetic.
- **Stale background** if anything repaints under an open box. There is no safety
  net by choice (see above). Keep the grep check in review.
- `Saving to server...` (main.c:157) draws over the already restored
  file-manager screen. That is fine.
- The static buffer costs 672 bytes of DGROUP, which is acceptable.

## Testing

Build with `make`. Drive headless MAME per `TESTING.md` and the IGC memory note:
`/root/sync/bldmame1/mame`, `vichd31.img`, a Lua step runner with `text=` VRAM
dumps and `shot`.

1. **Layout:** copy a multi-file selection C: to C:. Mid-copy: box at rows 8-12
   (title, name, bar, hint), row 23 blank. After: the screen matches a pre-copy
   dump except for the new files.
2. **Growth:** view a serial-panel file (`-rs232a pty` + igcfs). Expect 3 rows
   during `Fetching...`, then 4 during the transfer.
3. **Shrink:** delete a selection mixing a directory and plain files. The box
   goes to 5 rows during the directory and back to 4 for the files. The freed
   row shows the panel again, not border remnants.
4. **Hint change at unchanged percent:** in the same delete, the bar stays filled
   at the cached percent when the hint is removed.
5. **Mixed serial batch:** upload a folder plus a file. The hint shows during the
   folder and is gone for the file. ESC in the folder leads to
   `Serial transfer failed`, then the file still transfers.
6. **Reset/repaint:** after each of: F1 connect (Connecting -> Reading server),
   mkdir, disk-image ESC abort (`{combo={"ALT","ESC"}}`), a forced error
   mid-batch. Expect no box remnants. The next operation opens a clean box (no
   leftover hint, title or bar), and a serialfs.c:224-style error survives the
   main-loop repaint.
7. **Errors:** copy onto a write-protected floppy. The error is on row 23 while
   the box is up, and the next file's `ui_show_progress` blanks it.
8. **Overlapping dialog:** overwrite prompt over the box. After answering, the
   box is intact and the bar keeps its percent.
9. **Serial timing (FIFO):** at 38400 over MAME's PTY (private
   `cfg/victor9k.cfg`):
   - one 3-chunk file (big percent jumps, the worst-case tick) and one file over
     200 KB, both directions;
   - a DS image pushed and pulled.

   `cmp` all results, and expect no retries. Repeat on the real-hardware rig
   (`TESTING.md`). That is the authoritative check, since the tick gained a flag
   test and a variable row.
10. **Editor:** F4, F2. `File saved` on row 23, no box.
11. Update AGENTS.md (activity box, LIFO rule, hint ownership, `ui_notice`) and
    README if it mentions the status line.
