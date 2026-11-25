# IGC Code Review — Issues & Improvements

Review of the Victor 9000 DOS client (`igc/src/`) and the PC-side serial file server/tools (`igc/tools/serialfs/`).

---

## High-Impact Bugs

- [ ] **1. Zero-byte serial downloads hang for 30s and fail**
  - **Where:** `tools/serialfs/igcfs.py:454-485` vs `../viclibc2/filetrx/src/ftx_recv.c:92, 321, 386`
  - **Issue:** `_send_file()` computes `total = max(1, (len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE)`. For a 0-byte file, the server sends 1 empty `DataPacket` and waits `CREDIT_TIMEOUT` (30s) for a `READY` credit. On the Victor, `ftx_recv.c` computes `total_chunks = FTX_CHUNKS_NEEDED(0) == 0`, skips the data loop, ignores `FTX_CMD_DATA` while waiting for `END`, and never sends a credit. Even without flow control, `EndPacket(total_chunks=1)` fails `end->total_chunks != state->current_chunk`.
  - **Fix:** Remove `max(1, ...)` in `_send_file()` (matching `src/serialfs.c:347`), and add 0-byte upload/download coverage to `tools/serialfs/selftest.py` (which also uses `max(1, ...)` at line 91).

- [ ] **2. Sandbox escape gaps in `igcfs.py`**
  - **Where:** `tools/serialfs/igcfs.py:263-302, 414-429, 804-848`
  - **Issues:**
    - *Symlink escape on download (`L418-422`):* `_resolve_upload_dest()` (`L523`) and `handle_img_open()` (`L635`) call `.resolve()` on the final target and verify containment under `self.root`, but `handle_download()` does not — symlinks inside `--root` can expose arbitrary host files.
    - *`..` / empty final component in delete & rename (`L289-301`, `L804-848`):* `_resolve_dir_and_name()` only runs `_resolve()` on `parent_rel`. Passing `..` or `""` produces `self.root / ".."` (outside `--root`) or `self.root` itself.
    - *Windows drive-letter bypass (`igcfs.exe`):* None of the path resolvers reject `:` in components. On Windows, `Path("C:/root") / "D:secret.txt"` discards `self.root` entirely.
  - **Fix:** Reject empty, `.`, `..`, and `:` path components, and enforce a shared post-`resolve()` containment check (also disallowing `target == self.root` for delete/rename) in download, delete, mkdir, and rename.

- [ ] **3. `num_format()` groups thousands commas from the left instead of the right**
  - **Where:** `src/util.c:299-305`
  - **Issue:** `if (i > 0 && (digits - i) % 3 == 0)` formats `1234` as `"123,4"` and `10000` as `"100,00"`. Affects file sizes, file counts, editor `L:/C:` display, and multi-file dialogs.
  - **Fix:** Change `(digits - i) % 3 == 0` to `i % 3 == 0`.

- [ ] **4. Stack overflow risk in recursive directory transfers (`-s` disables stack probes)**
  - **Where:** `src/serialfs.c:558, 719-792`, `src/fileops.c:19, 265-369`, `Makefile:44, 109`
  - **Issue:** `igc.exe` links with `OPTION stack=8192` and compiles with `-s` (no stack-overflow checks). `sfs_put_tree_r()` allocates three `char[MAX_FULL_PATH]` (252 B) + `DTA` (43 B) + locals (~312 B/frame). At `SFS_TREE_MAX_DEPTH = 24`, recursion alone uses ~7.5 KB before adding `fops_copy_serial` (~170 B), `serialfs_put_named` (~160 B), `pkt_send`, and hardware IRQs. `fops_copy_dir()` is also tight (~240 B × 24 ≈ 5.8 KB plus `fops_copy_file` + `dlg_overwrite`).
  - **Fix:** Reuse `src_child` for `pattern` in `sfs_put_tree_r()` (saves 84 B/level immediately) and lower max recursion depth (e.g. 12–16) or move path scratch off the stack.

- [ ] **5. Text editor bugs (`src/editor.c`)**
  - **Signed `char` clobbers extended bytes (`L375-378`):** `Makefile` compiles with `-j` (signed `char`), so `if (c < 32) c = '.';` turns all bytes `>= 0x80` (box-drawing/extended Victor characters) into `'.'`. Cast `(uint8_t)c < 32`.
  - **No horizontal scroll while typing (`L566-607`):** `insert_char()` increments `cursor_col` without calling `scroll_if_needed()`, so typing past column 78 types off-screen invisibly until a navigation key is pressed.
  - **Unsigned cursor underflow on Ctrl-K (`L766-775`):** Cutting the last line when `cursor_line == top_line` decrements `cursor_line` below `top_line` without calling `scroll_if_needed()`, causing `update_cursor()` to compute `cursor_line - top_line` as `0xFFFF`.
  - **Silent line-table truncation (`L158-165`):** If a file fits in `buf_size` bytes but exceeds `max_lines` (e.g. >128 lines on `MEM_TINY`), `load_file()` does not set `truncated = TRUE`; remaining lines glue onto the last line.
  - **Destructive / misleading `save_file()` and `editor_edit()` (`L239-268`, `L1033-1054`):** `dos_create` truncates the existing file in-place before writing (disk-full mid-save destroys the original; read-only files fail with `"Save failed - disk full?"`), and `editor_edit()` treats *any* `load_file()` error (e.g. drive not ready / access denied) as opening a new empty file.

- [ ] **6. Upload read-error causes protocol desync**
  - **Where:** `src/serialfs.c:339-424`
  - **Issue:** In the CRC pre-read loop (`L340`), a negative return from `ftx_file_read()` is treated as EOF, computing a wrong CRC and starting a doomed upload. Mid-upload (`L395`), if `ftx_file_read() <= 0`, `serialfs_put_named()` returns `-1` without sending `FTX_CMD_ABORT` or `FTX_CMD_END(status=ERROR)`. `igcfs.py` remains blocked in `_recv_after_start()` and consumes the next request (e.g. `LIST`) as an unexpected packet.
  - **Fix:** Abort before `START` if the CRC pre-read errors; send `FTX_CMD_END(status=FTX_STATUS_ERROR)` or `FTX_CMD_ABORT` if a read fails mid-upload.

---

## Medium Bugs & Edge Cases

- [ ] **7. Selection marker invisible under cursor & `Space` on last entry does not redraw**
  - **Where:** `src/ui.c:254-257`, `src/main.c:418-428`
  - **Issue:** `if (f->selected && !is_cursor)` hides `*` whenever the cursor is on a selected file (so right-click toggling produces no visual change until moving away). Pressing `Space` on the final file leaves `old_cursor == p->cursor`, so `ui_update_cursor()` skips redrawing.
  - **Fix:** Draw `*` with `attr` whenever `f->selected` is set, and redraw the toggled row even if the cursor did not advance.

- [ ] **8. Truncation indicator overwrites the 19th visible file row**
  - **Where:** `src/ui.c:305-307`
  - **Issue:** `ROW_FILES_END` (row 20) is also the last visible file row (`ROW_FILES_START + PANEL_HEIGHT - 1`). Drawing `"[...more files]"` overwrites that file entry (and is erased again on cursor movement).
  - **Fix:** Indicate truncation on the bottom border (e.g. `1,024+ files`).

- [ ] **9. 12-character directory names truncated to 11 characters in panels**
  - **Where:** `src/ui.c:261-265`
  - **Issue:** `str_copy_n(&dir_name[1], f->name, 12)` copies at most 11 chars + NUL; `dir_name[16]` has room for `13`.

- [ ] **10. File operations edge cases (`src/fileops.c`)**
  - **F5 Rename mismatch (`L680-725`):** When 1 file is selected while the cursor sits on a different file, F5 displays the selected file's name in `"File Operation"`, then choosing `(R)ename` renames the *cursor* file instead.
  - **Case-only rename fails on DOS (`L1112`):** `str_cmp(old_name, new_name) == 0` is case-sensitive, so renaming `FOO.TXT` → `foo.txt` invokes `dos_rename` and fails with `"Cannot rename file"`.
  - **Copying a directory onto an existing regular file (`L290-296`, `src/serialfs.c:656`):** When `dos_mkdir` fails, `dos_exists` only tests existence rather than `DOS_ATTR_DIRECTORY`, so recursion proceeds and fails on every child.
  - **Overwriting read-only files fails:** Confirmed overwrites fail on read-only local files in serial download (`fops_copy_serial` → `ftx_file_create`) and local `diskimg_read_to_file()` (`dos_create`), and on Windows in `igcfs.py` (`os.replace()` raises `PermissionError` over a read-only target).
  - **Stale error banners left on row 23:** `src/main.c:146-148, 160-161, 211-214` and `src/fileops.c:747-749, 837-839, 1061-1063, 1073-1075, 1121-1123, 1134-1136` call `ui_error(...); kbd_wait();` without `ui_clear_status()`.
  - **Serial tree cancel & double-error prompts (`L598-602`, `src/serialfs.c:633-870`):** Pressing `Esc` inside `sfs_*_tree_r` returns `-1`, which `fops_copy_serial` treats as `"Serial transfer failed"` and keeps looping over remaining selected items. Tree errors also display two `ui_error + kbd_wait` prompts.
  - **Ignored delete results & `dos_rename` guard in `fops_move()` (`L866-935`):** Return values of `fops_delete_dir`/`fops_delete_file`/`serialfs_delete*` are ignored (an `Esc` cancel during delete does not stop a batch move), and `path_is_inside()` should be checked before same-drive `dos_rename()`.

- [ ] **11. Mouse double-click midnight wrap bug**
  - **Where:** `src/mouse.c:240-257`
  - **Issue:** When `now < g_last_time`, `now += 8640000L` is stored back into `g_last_time`, leaving `g_last_time` permanently shifted forward by 24h after midnight.
  - **Fix:** Store raw `dos_hundredths()` in `g_last_time` and apply the wrap only when computing the elapsed delta.

- [ ] **12. `panel_change_dir()` / `panel_go_parent()` mutate `p->path` before reading**
  - **Where:** `src/panel.c:309-315, 327-331`
  - **Issue:** If `panel_read_dir(p)` fails (e.g. serial disconnect or missing directory), `p->path` is already overwritten with the failed path and shows an empty panel.

- [ ] **13. Protocol command byte collision with `viclibc2`**
  - **Where:** `src/serialfs_proto.h:14-15`, `tools/serialfs/igcfs.py:66-67` vs `../viclibc2/filetrx/include/ftx_protocol.h:29-30`
  - **Issue:** `igc` assigns `0x1A` (`SFS_CMD_DELETE`) and `0x1B` (`SFS_CMD_MKDIR`), which `viclibc2` now uses for `FTX_CMD_QUERY` and `FTX_CMD_HAVE`. Doesn't collide today because `igc` doesn't link `ftx_server.c`, but should be kept disjoint if segmented transfers are adopted.

- [ ] **14. README vs code mismatch on `Enter`**
  - **Where:** `README.md:39` vs `src/main.c:71`
  - **Issue:** `README.md` documents `Enter | Open directory or view file`, but `handle_enter()` still has `/* TODO: For files, could launch viewer/editor */` (whereas mouse double-click already invokes View/F3).

- [ ] **15. `igcfs.py` & `v9kfiletrx` cross-platform/protocol quirks**
  - `_short_name()` (`tools/serialfs/igcfs.py:186`) forces `ext = ''` for directories, mangling valid DOS 8.3 directories like `SRC.OLD` into `SRCOLD~1`.
  - `handle_rename()` (`tools/serialfs/igcfs.py:843`) uses `Path.rename()`, which silently overwrites existing files on POSIX but raises `FileExistsError` on Windows (and differs from DOS `INT 21h AH=56h`).
  - `handle_download()` (`tools/serialfs/igcfs.py:428`) uses `os.path.basename(rel)`, which does not split `\` on Linux (`SUB\INNER.TXT` stays unsplit in `START.filename`).
  - `-v` / `--verbose` (`tools/serialfs/igcfs.py:874`) is stored in `self.verbose` but never checked — all `log()` calls print unconditionally.
  - `_req_path()` (`L356`) and `handle_rename()` (`L832`) index `payload` without length checks; a short packet raises `IndexError` and sends no error reply.
  - `_unix_to_dos_datetime()` (`tools/serialfs/v9kfiletrx/filetrx.py:459`) wraps pre-1980 timestamps (`tm_year < 1980`) to year ~2098 instead of clamping to 1980.

---

## Performance, UX & Maintenance Improvements

- [ ] **16. Eliminate 4× `dos_get_free_space()` calls per `Tab` / panel click**
  - **Where:** `src/main.c:334-335, 402-406, 454-458`, `src/ui.c:146`
  - **Issue:** Switching panels calls `ui_draw_title_bar()` immediately *and* sets `g_need_redraw = TRUE`, causing `main_loop()` to call `ui_draw_title_bar()` a second time. Each call queries `INT 21h AH=36h` on both drives (4 floppy probes per panel switch).
  - **Fix:** Cache `free_kb` on `Panel` (updated in `panel_read_dir`) and drop the duplicate `ui_draw_title_bar()` call.

- [ ] **17. Send full server-relative paths for single-file uploads and serial disk images**
  - **Where:** `src/serialfs.c:445-450`, `src/diskimg.c:596-598`
  - **Issue:** Single-file uploads strip `remote_rel` to `path_basename(remote_rel)` and `diskimg_read_to_file()` sends a bare name, relying on `igcfs.py`'s `self.cwd` (last `LIST` directory) even though `_resolve_upload_dest()` already supports root-relative paths. Passing `serialfs_build_rel()` directly removes `cwd` drift risk.

- [ ] **18. Allow cancelling single-file serial transfers with `Esc` (and exiting View mode with `Esc`)**
  - **Where:** `src/serialfs.c:248-253, 390-424`, `src/editor.c:887-923`
  - **Issue:** Large serial transfers at 38400 baud cannot be aborted mid-file, and `Esc` does not exit read-only View mode (F3).

- [ ] **19. Add mouse click support to `dlg_file_op()` and `dlg_overwrite()`**
  - **Where:** `src/dialog.c:686-720, 743-790`
  - **Issue:** Unlike `dlg_confirm`, `dlg_input`, `dlg_drive_select`, and `dlg_menu`, neither dialog handles `KEY_MOUSE` clicks.

- [ ] **20. Drop redundant 4 KB `g_screen_save` buffer in `editor.c`**
  - **Where:** `src/editor.c:102, 999, 1011`, `src/main.c:241-244, 261-264`
  - **Issue:** `main.c` already redraws the entire IGC UI after `editor_view()` / `editor_edit()` returns; removing `g_screen_save` frees 4 KB of conventional RAM on low-memory machines.

- [ ] **21. Build & defensive cleanups**
  - Add `$(VICLIBC)/serial/include/*.h` and `$(VICLIBC)/filetrx/include/*.h` as prerequisites for `vic_*.obj`, `serialfs.obj`, and `diskimg.obj` in `Makefile:116-128`.
  - Guard `mem_alloc()` (`src/mem.c:98-106`) against `bytes == 0` or `bytes > 0xFFFF0UL` (16-bit paragraph overflow).
  - Call `serialfs_disconnect()` on normal exit and `mouse_shutdown()` on early startup error exits in `src/main.c:504-553`.
  - Guard `dlg_menu()` (`src/dialog.c:579`) against `count == 0` and clamp `w <= 78`.
