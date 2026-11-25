# AGENTS.md

Instructions for any coding agent (Claude Code, Codex, Grok Build, ...) working in this repository.
Read at the start of every session, so keep it short: one line per fact.

Before the final message of any task that changed repo files or ran the project's
build/test/run commands, apply at most one AGENTS.md change and report it with one
`AGENTS.md change` block, or the line `AGENTS.md: no change.` See "Continuous improvement" below.

## Project

- Purpose: Misc software for the Victor 9000: igc (serial file transfer client) and its PC-side server igcfs, key-remover, mem, mouse, v9kdiag, viclibc2.
- Stack: 8086 C/asm for the Victor 9000 side (per-directory makefiles); Python 3 + pyserial for igcfs; PyInstaller for the shipped igcfs.exe.
- Build: igcfs.exe must be built on Windows (PyInstaller cannot cross-compile). From a Prime session on the Linux box: `win11 run --cwd igc/tools/serialfs .\build.bat` (the Windows 11 VM; run `win11 help` first), then copy `igc/tools/serialfs/dist/igcfs.exe` over the tracked `igc/tools/serialfs/igcfs.exe` and commit it. Linux: `igc/tools/serialfs/build.sh`.
- Test: `python igc/tools/serialfs/selftest.py` (Linux or `win11 run --cwd igc/tools/serialfs python selftest.py`); igc end-to-end in MAME: `python3 igc/tools/mame/test_igc.py` (needs the ROMs, boot floppy and tools listed in igc/tools/mame/README.md).
- Run: `igcfs.exe --root C:\victor --dev COM1 --baud 38400 -v` (Windows) or `igcfs.py --root <dir> --dev /dev/ttyUSB0` (Linux); see igc/README.md.
- Layout: `igc/` = client + `tools/serialfs/` (igcfs.py, build.bat/.sh, igcfs.spec, dist/) · `key-remover/`, `mem/`, `mouse/`, `v9kdiag/`, `viclibc2/` = one program each

## Working rules

- Prefer the simplest solution that works. If a simpler approach exists, propose it first.
- Do not change behaviour outside the task's scope without saying so.
- Before reporting done: if Build/Test above are real commands (not placeholders), run them and
  quote any failure verbatim. Say which checks were not run.

## Lessons learned

- igc build on this box: `make WATCOM=/root/tools/openwatcom2` (the Makefile's default /opt/watcom does not exist here).
- igc MAME tests `escdl` and `view` fail now and then on unchanged code (escdl: the Esc misses the download); rerun them alone before blaming a change.

## Continuous improvement

**When:** after a top-level task (not a subagent, not a question, not search-only work) that
changed repo files or ran the project's build/test/run commands. Applying an AGENTS.md change
does not itself trigger another retrospective.

**Review** what this task taught about this project: a command, flag, path or setup step found
by trial and error; a tool, library or hardware quirk that cost time; a convention the user
stated or corrected; a line in this file that proved wrong, stale or redundant.

**Filter.** Record only a fact that is useful on a future task in this repo, not already in
this file, and not obvious from the README, manifest or file names (a buried gotcha is fine).
A user correction is always eligible, even if it is now visible in code. Filling or fixing a
Project placeholder is eligible when the fact is known. Never record global style preferences,
secrets, or one-off task details. At most one change per task; prefer `change` or `remove`
over `add`.

**Apply, then report.** Edit this file directly; do not ask first. End the final message with exactly one of:

```
AGENTS.md change
Section: <Project | Working rules | Lessons learned>
Action: <add | change | remove>
Replaces: <exact existing line; "(none)" for add>
Line: <exact new line; "(none)" for remove>
Why: <one sentence: what happened that makes this worth keeping>
```

or the single line `AGENTS.md: no change.`

**Applying.** Put the line in the named section (merge, do not append at the bottom). If the
user rejects or rewords a change, revert or fix it at once, and never re-add a rejected item
unless new evidence appears. Commit it with the task's other changes, or on its own if they
are already committed.

**Size.** Lessons learned holds at most 40 entries and Project at most 10 lines, one line each.
If a change would exceed that, make a removal in the same change. If a project needs
more, the overflow belongs in the README, not here.
