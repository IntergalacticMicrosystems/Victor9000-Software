#!/usr/bin/env python3
"""test_igc.py - igc end-to-end tests in MAME (see README.md).

    python3 test_igc.py [WORK_DIR] [TEST ...]     # default: all tests

Each test boots a fresh Victor: A: = DOS 3.1 boot floppy, B: = data floppy
with IGC.EXE, Serial A = igcfs.py serving WORK_DIR/<test>/root. igc runs from
B:, so its temp files land there. The left panel is switched to Serial A.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from v9kmame import Victor  # noqa: E402

WORK = "/srv/prime/work/v9kmame-tests"
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print("  [%s] %s%s" % ("ok" if ok else "XX", name, (" - " + detail) if detail and not ok else ""))


def boot(name, b_files=None, server_files=None, throttle=False, prepare=None,
         mouse=False):
    """Fresh Victor running igc, left panel on Serial A, cursor on top."""
    vm = Victor(os.path.join(WORK, name), b_files=b_files, server_files=server_files)
    if prepare:
        prepare(vm)
    vm.start(throttle=throttle, mouse=mouse)
    if mouse:
        vm.load_mouse()
    vm.post("b:\r")
    vm.post("igc\r")
    vm.wait_text("IGC.EXE", timeout=120)
    vm.press("F1")
    vm.wait_text("Serial A")
    vm.press("HOME")                    # the menu opens on the current drive
    vm.press("DOWN")
    vm.press("DOWN")
    vm.press("ENTER")
    vm.wait_text("SERA:", timeout=60)
    time.sleep(1)
    return vm


def top(vm):
    """Cursor to the first entry (tests name the target so it sorts first)."""
    vm.press("HOME")


def quit_igc(vm):
    """F7 and confirm, then wait for DOS, so everything igc wrote is on the
    floppy image before MAME stops."""
    vm.press("F7")
    vm.wait_text("exit", timeout=30)
    vm.post("y")
    vm.wait_text("B:\\>", timeout=60)


def server(vm, rel):
    with open(os.path.join(vm.root, rel), "rb") as f:
        return f.read()


def col40_clean(text):
    rows = text.split("\n")
    return all(len(r) > 40 and r[40] == " " for r in rows[2:21])


def t_download():
    """Download over an existing B: file: one prompt, file replaced."""
    print("download over an existing file")
    data = bytes(range(256)) * 40                       # 10 KB
    vm = boot("download", b_files={"A.DAT": b"old"}, server_files={"A.DAT": data})
    try:
        top(vm)
        vm.press("F5")
        vm.wait_text("(C)opy")
        vm.post("c")
        vm.wait_text("File Exists", timeout=30)
        vm.post("y")
        vm.wait_log("-> sent")
        time.sleep(5)                                   # verify + commit
        screen = vm.screen()
        check("no error shown", "ERROR" not in screen, "\n" + screen)
        quit_igc(vm)
    finally:
        vm.stop()
    check("B:A.DAT replaced with the server copy", vm.get_b("A.DAT") == data)


def t_upload():
    """Upload a B: file to the server."""
    print("upload")
    data = b"uploaded line\r\n" * 300
    vm = boot("upload", b_files={"UP.TXT": data})
    try:
        vm.press("TAB")                                 # to the B: panel
        time.sleep(1)
        vm.press("HOME")
        vm.press("DOWN")                                # IGC.EXE, UP.TXT
        vm.press("F5")
        vm.wait_text("UP.TXT")
        vm.post("c")
        vm.wait_text("1 files", timeout=5)              # wait for refresh below
        end = time.monotonic() + 120
        while time.monotonic() < end and not os.path.exists(os.path.join(vm.root, "UP.TXT")):
            time.sleep(0.5)
        time.sleep(2)
    finally:
        vm.stop()
    ok = os.path.exists(os.path.join(vm.root, "UP.TXT")) and server(vm, "UP.TXT") == data
    check("server UP.TXT matches", ok)


def t_view_column40():
    """F3 on a server file with long lines, Esc: panels repainted cleanly."""
    print("view a long-line file, then Esc")
    text = b"".join(b"%02d " % i + b"x" * 70 + b"\r\n" for i in range(30))
    vm = boot("view", server_files={"LONG.TXT": text})
    try:
        top(vm)
        vm.press("F3")
        shown = vm.wait_text("[View]", timeout=60)
        check("viewer shows the file", "00 xxxx" in shown)
        vm.press("ESC")
        after = vm.wait_text("SERA:", timeout=30)
        check("Esc leaves the viewer", "LONG.TXT" in after)
        check("column 40 clean after the viewer", col40_clean(after),
              "\n" + after)
    finally:
        vm.stop()


def make_readonly(vm):
    # igcfs marks a file read-only when it is not writable to igcfs, and
    # root can write a 0444 file, so make it immutable instead
    path = os.path.join(vm.root, "RO.TXT")
    os.chmod(path, 0o444)
    subprocess.run(["chattr", "+i", path], check=True)


def t_edit_readonly_server():
    """F4 on a read-only server file opens it for viewing."""
    print("F4 on a read-only server file")
    vm = boot("readonly", server_files={"RO.TXT": b"keep me\r\n"},
              prepare=make_readonly)
    try:
        top(vm)
        vm.press("F4")
        shown = vm.wait_text("Read-Only File", timeout=60)
        check("read-only notice", "Opened for viewing" in shown)
        vm.press("ENTER")
        vm.wait_text("keep me", timeout=20)
        vm.press("ESC")
        vm.wait_text("SERA:", timeout=30)
        time.sleep(2)
    finally:
        vm.stop()
        subprocess.run(["chattr", "-i", os.path.join(vm.root, "RO.TXT")])
    check("server file unchanged", server(vm, "RO.TXT") == b"keep me\r\n")


def t_error_banner():
    """An error prompt clears its banner after the key."""
    print("error banner clears")
    vm = boot("banner")
    try:
        vm.press("TAB")                                 # B: panel
        time.sleep(1)
        vm.press("F2")
        vm.wait_text("Mkdir", timeout=20)
        vm.post("IGC.EXE\r")                            # a file of that name
        shown = vm.wait_text("ERROR", timeout=20)
        check("error shown", "Cannot create directory" in shown, shown.split("\n")[22])
        vm.press("SPACE")
        time.sleep(1)
        row = vm.screen().split("\n")[22]
        check("banner gone after the key", row.strip() == "", repr(row))
    finally:
        vm.stop()


def t_esc_download():
    """Esc during a large download keeps the original file."""
    print("Esc during a download")
    data = os.urandom(120000)
    vm = boot("escdl", b_files={"BIG.DAT": b"original"}, server_files={"BIG.DAT": data})
    try:
        top(vm)
        vm.press("F5")
        vm.wait_text("(C)opy")
        vm.post("c")
        vm.wait_text("File Exists", timeout=30)
        vm.post("y")
        vm.wait_log("<- getfile BIG.DAT")
        time.sleep(2)
        vm.press("ESC")
        end = time.monotonic() + 120
        while time.monotonic() < end and "Copying" in vm.screen():
            time.sleep(0.5)
        screen = vm.screen()
        check("no error after Esc", "ERROR" not in screen, screen.split("\n")[22])
        check("link still lists", "BIG.DAT" in screen)
        quit_igc(vm)
    finally:
        vm.stop()
    check("B:BIG.DAT still the original", vm.get_b("BIG.DAT") == b"original")


def t_mouse_dialogs():
    """Item 19: clicks on the F5 and File Exists choices act as their keys."""
    print("mouse clicks in the F5 and overwrite dialogs")
    data = b"from the server\r\n" * 50
    vm = boot("mouse", b_files={"M.TXT": b"old"}, server_files={"M.TXT": data},
              mouse=True)
    try:
        top(vm)
        vm.press("F5")
        vm.wait_text("(C)opy")
        col, row = vm.find("(C)opy")
        vm.click(col + 3, row)                          # on "opy"
        shown = vm.wait_text("File Exists", timeout=30)
        check("click on (C)opy starts a copy", "(Y)es" in shown)
        col, row = vm.find("(Y)es")
        vm.click(col + 1, row)                          # on the "Y"
        vm.wait_log("-> sent")
        time.sleep(5)
        screen = vm.screen()
        check("no error after the clicks", "ERROR" not in screen, "\n" + screen)
        quit_igc(vm)
    finally:
        vm.stop()
    check("click on (Y)es replaced B:M.TXT", vm.get_b("M.TXT") == data)


TESTS = {"download": t_download, "upload": t_upload, "view": t_view_column40,
         "readonly": t_edit_readonly_server, "banner": t_error_banner,
         "escdl": t_esc_download, "mouse": t_mouse_dialogs}

if __name__ == "__main__":
    args = sys.argv[1:]
    if args and os.sep in args[0]:
        WORK = args.pop(0)
    for name in (args or list(TESTS)):
        try:
            TESTS[name]()
        except Exception as e:                          # report and go on
            check(name + " ran", False, "%s: %s" % (type(e).__name__, e))
    failed = [n for n, ok in results if not ok]
    print("\n%d checks, %d failed%s" % (len(results), len(failed),
                                        (": " + ", ".join(failed)) if failed else ""))
    sys.exit(1 if failed else 0)
