#!/usr/bin/env python3
"""v9kmame.py - run igc in MAME's victor9k and drive it from Python.

    vm = Victor(work_dir)                    # fresh A:/B: images, no MAME yet
    vm.start(serial=True)                    # boot, igcfs on Serial A (38400)
    vm.post("b:\\r"); vm.post("igc\\r")
    vm.wait_text("SERA", timeout=60)         # emulated-screen text search
    vm.press("F3"); print(vm.screen())
    vm.snap("after-f3")                      # PNG in work_dir/snaps/
    vm.stop()

Needs MAME with the victor9k ROM set, a bootable DOS 3.1 floppy and
vtg_image_util (see README.md). Paths default to the locations below and can
be overridden with the environment variables of the same name.
"""
import glob
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

MAME = os.environ.get("V9K_MAME", "/root/tools/bin/mame")
ROMPATH = os.environ.get("V9K_ROMPATH", "/root/tools/mame-victor/roms")
BOOT_IMG = os.environ.get("V9K_BOOT_IMG", "/root/tools/mame-victor/V9K31.orig.IMG")
VTG = os.environ.get("V9K_VTG", "/root/tools/vtg_image_util")
# ctmouse_v9k's serial-mouse feeder (Microsoft protocol into MAME's pty)
MOUSE_TOOLS = os.environ.get("V9K_MOUSE_TOOLS", "/srv/prime/repos/ctmouse_v9k/tools")
IGC_EXE = os.environ.get("V9K_IGC_EXE", os.path.join(REPO, "igc", "bin", "igc.exe"))
IGCFS = os.path.join(REPO, "igc", "tools", "serialfs", "igcfs.py")

# Short names for the keys whose MAME field names are long (ctl/keys.txt)
KEYS = dict({"F%d" % i: "F%d [%d]" % (i, i) for i in range(1, 11)},
            UP="\u2191/Brightness Up \u263c\u25b4",
            DOWN="\u2193/Brightness Down \u263c\u25be",
            LEFT="\u2190/Contrast Up \u25d0\u25b4",
            RIGHT="\u2192/Contrast Down \u25d0\u25be",
            PGUP="Word \u2190/Volume Up \U0001f508\u25b4",
            PGDN="Word \u2192/Volume Down \U0001f508\u25be",
            TAB="TAB/BACK", ENTER="RETURN",
            SPACE="Spacebar", HOME="CLR/HOME", END="ERASE/EOL",
            BS="BACKSPACE", DEL="DEL", INS="INS")

# Esc is the front (Alt) legend of the RVS key. MAME's natural keyboard maps
# chr(27) to the bare key, which Victor DOS does not read as Esc.
CHORDS = {"ESC": ("ALT", "RVS On-Off/ESC")}

# MAME rs232 slot settings (src/devices/bus/rs232/rs232.h)
BAUD_38400 = 11
BAUD_1200 = 4
DATABITS_7 = 2


def vtg(*args):
    """Run vtg_image_util; raises on failure."""
    subprocess.run([sys.executable, "-m", "vtg_image_util", "-q", *args],
                   cwd=VTG, check=True, stdout=subprocess.DEVNULL)


def make_blank(path):
    """An empty data floppy. vtg_image_util's "create" writes a PC-style boot
    sector that Victor DOS cannot read, so empty a copy of the boot disk."""
    import json
    shutil.copyfile(BOOT_IMG, path)
    out = subprocess.run([sys.executable, "-m", "vtg_image_util", "--json",
                          "list", path], cwd=VTG, check=True,
                         capture_output=True, text=True).stdout
    for entry in json.loads(out)["files"]:
        if "R" in entry["attr"]:
            vtg("attr", "--", path + ":\\" + entry["name"], "-R", "-H", "-S")
        vtg("delete", "-r", path + ":\\" + entry["name"])


class Victor:
    def __init__(self, work, b_files=None, server_files=None):
        """Fresh work dir: A: = boot floppy, B: = blank floppy holding
        IGC.EXE and b_files ({name: bytes}); the server root gets
        server_files ({relpath: bytes})."""
        self.work = os.path.abspath(work)
        if os.path.isdir(self.work):
            subprocess.run(["chattr", "-R", "-i", self.work], stderr=subprocess.DEVNULL)
            shutil.rmtree(self.work)
        for sub in ("ctl", "snaps", "root", "mame"):
            os.makedirs(os.path.join(self.work, sub))
        self.ctl = os.path.join(self.work, "ctl")
        self.root = os.path.join(self.work, "root")
        self.a_img = os.path.join(self.work, "a.img")
        self.b_img = os.path.join(self.work, "b.img")
        shutil.copyfile(BOOT_IMG, self.a_img)
        make_blank(self.b_img)
        vtg("copy", IGC_EXE, self.b_img + ":\\IGC.EXE")
        for name, data in (b_files or {}).items():
            self.put_b(name, data)
        for rel, data in (server_files or {}).items():
            path = os.path.join(self.root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        self.seq = 0
        self.mame = self.igcfs = self.mouse = None
        self.pty = None

    # -- disk images (only while MAME is stopped) ---------------------------
    def put_b(self, name, data):
        tmp = os.path.join(self.work, "upload.tmp")
        with open(tmp, "wb") as f:
            f.write(data)
        vtg("copy", tmp, self.b_img + ":\\" + name)
        os.remove(tmp)

    def get_b(self, name):
        tmp = os.path.join(self.work, "download.tmp")
        if os.path.exists(tmp):
            os.remove(tmp)
        vtg("copy", self.b_img + ":\\" + name, tmp)
        with open(tmp, "rb") as f:
            data = f.read()
        os.remove(tmp)
        return data

    # -- MAME ----------------------------------------------------------------
    def start(self, serial=True, throttle=False, boot_timeout=120, mouse=False):
        """Boot to A:\\>. serial: igcfs on Serial A at 38400. mouse: a
        Microsoft serial mouse on Serial B (load it with load_mouse)."""
        before = set(glob.glob("/dev/pts/[0-9]*"))
        cmd = [MAME, "victor9k", "-rompath", ROMPATH, "-ramsize", "896K",
               "-flop1", self.a_img, "-flop2", self.b_img,
               "-video", "none", "-sound", "none", "-skip_gameinfo",
               "-snapshot_directory", os.path.join(self.work, "snaps"),
               "-cfg_directory", os.path.join(self.work, "mame"),
               "-nvram_directory", os.path.join(self.work, "mame"),
               "-autoboot_script", os.path.join(HERE, "v9k.lua")]
        # Serial settings via the config file: changing the fields while
        # running makes MAME reopen the pty, and igcfs then reads EIO
        ports = []

        def port(slot, name, default, value):
            ports.append('<port tag=":%s:pty:RS232_%s" type="CONFIG" mask="255" '
                         'defvalue="%d" value="%d" />' % (slot, name, default, value))
        if serial:
            cmd += ["-rs232a", "pty"]
            port("rs232a", "TXBAUD", 7, BAUD_38400)
            port("rs232a", "RXBAUD", 7, BAUD_38400)
        if mouse:
            cmd += ["-rs232b", "pty"]
            port("rs232b", "TXBAUD", 7, BAUD_1200)
            port("rs232b", "RXBAUD", 7, BAUD_1200)
            port("rs232b", "DATABITS", 3, DATABITS_7)
        with open(os.path.join(self.work, "mame", "victor9k.cfg"), "w") as f:
            f.write('<?xml version="1.0"?><mameconfig version="10">'
                    '<system name="victor9k"><input>%s</input></system>'
                    '</mameconfig>\n' % "".join(ports))
        if not throttle:
            cmd.append("-nothrottle")
        env = dict(os.environ, V9K_CTL=self.ctl)
        self.mame_log = open(os.path.join(self.work, "mame.log"), "w")
        self.mame = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL,
                                     stdout=self.mame_log, stderr=subprocess.STDOUT)
        self._wait(lambda: os.path.exists(os.path.join(self.ctl, "ready")), 30,
                   "MAME did not start (see mame.log)")
        new = sorted(set(glob.glob("/dev/pts/[0-9]*")) - before)
        if len(new) != serial + mouse:
            raise RuntimeError("cannot find MAME's ptys: %r" % new)
        if serial and mouse:
            # MAME opens rs232a's pty first; mame_ptys lists them in fd order
            sys.path.insert(0, MOUSE_TOOLS)
            import v9kmouse
            ordered = [p for p in v9kmouse.mame_ptys() if p in new]
            if len(ordered) != 2:
                raise RuntimeError("cannot order MAME's ptys: %r" % ordered)
            new = ordered
        if mouse:
            sys.path.insert(0, MOUSE_TOOLS)
            import v9kmouse
            self.mouse = v9kmouse.Mouse(v9kmouse.Port(new[-1]))
            self.mouse_x = self.mouse_y = None
        if serial:
            self.pty = new[0]
            self.igcfs_log = open(os.path.join(self.work, "igcfs.log"), "w")
            self.igcfs = subprocess.Popen(
                [sys.executable, IGCFS, "--root", self.root, "--dev", self.pty,
                 "--baud", "38400", "-v"],
                env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"),
                stdin=subprocess.DEVNULL, stdout=self.igcfs_log,
                stderr=subprocess.STDOUT)
        self.wait_text("A:\\>", timeout=boot_timeout, raw=True)

    def stop(self):
        if self.mame and self.mame.poll() is None:
            try:
                self.cmd("exit", timeout=10)
            except Exception:
                pass
            try:
                self.mame.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.mame.kill()
        if self.igcfs and self.igcfs.poll() is None:
            self.igcfs.terminate()
            try:
                self.igcfs.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.igcfs.kill()
        self.mame = self.igcfs = None

    # -- commands ------------------------------------------------------------
    def _wait(self, cond, timeout, what):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if cond():
                return
            if self.mame and self.mame.poll() is not None:
                raise RuntimeError("MAME exited: " + what)
            time.sleep(0.05)
        raise TimeoutError(what)

    def cmd(self, line, timeout=30):
        self.seq += 1
        path = os.path.join(self.ctl, "cmd.%d" % self.seq)
        with open(path + ".tmp", "w") as f:
            f.write(line + "\n")
        os.rename(path + ".tmp", path)
        ack = os.path.join(self.ctl, "ack.%d" % self.seq)
        self._wait(lambda: os.path.exists(ack), timeout, "no answer to " + line)
        with open(ack) as f:
            reply = f.read()
        os.remove(ack)
        if reply.startswith("error:"):
            raise RuntimeError(reply)
        return reply

    def idle(self, timeout=60):
        self._wait(lambda: self.cmd("kbidle") == "1", timeout, "keyboard busy")

    def post(self, text):
        """Type text (use \\r for Return) and wait until it is all typed."""
        self.cmd("post " + text.replace("\r", "\\r"))
        self.idle()

    def press(self, key, frames=4):
        """Press one key by its MAME field name (see ctl/keys.txt)."""
        if key in CHORDS:
            return self.chord(*CHORDS[key], frames=frames)
        self.cmd("press %s %d" % (KEYS.get(key, key), frames))
        self.idle()

    def chord(self, mod, key, frames=4):
        """Hold two keys together, e.g. chord("Left Shift", "ESC")."""
        self.cmd("chord %s|%s %d" % (KEYS.get(mod, mod), KEYS.get(key, key), frames))
        self.idle()

    def screen(self):
        return self.cmd("screen")

    def wait_text(self, text, timeout=60, raw=False):
        """Wait (real seconds) until text is on the screen; returns it."""
        last = [""]

        def seen():
            last[0] = self.screen()
            return text in last[0]
        try:
            self._wait(seen, timeout, "text %r" % text)
        except TimeoutError:
            raise TimeoutError("text %r not on screen:\n%s" % (text, last[0]))
        return last[0]

    def wait_log(self, text, timeout=120):
        """Wait (real seconds) until igcfs has logged text."""
        path = os.path.join(self.work, "igcfs.log")

        def seen():
            self.igcfs_log.flush()
            with open(path) as f:
                return text in f.read()
        self._wait(seen, timeout, "igcfs log %r" % text)

    # -- mouse (start(mouse=True)) -------------------------------------------
    def load_mouse(self, timeout=60):
        """Install CTMOUSEV on Serial B: it probes for the mouse, so send the
        Microsoft ID while the command runs."""
        import threading
        done = threading.Event()

        def hello():
            while not done.is_set():
                self.mouse.hello("M")
                time.sleep(0.2)
        t = threading.Thread(target=hello, daemon=True)
        t.start()
        try:
            self.post("a:ctmousev /sb\r")
            self.wait_text("Installed at", timeout=timeout)
        finally:
            done.set()
            t.join()

    def _emu_wait(self, seconds):
        """Let seconds of emulated time pass (mouse bytes go at 1200 baud)."""
        t0 = float(self.cmd("time"))
        self._wait(lambda: float(self.cmd("time")) >= t0 + seconds, 120, "emulated time")

    def _steps(self, dx, dy):
        # Moves of up to 4 mickeys bypass CuteMouse's acceleration, and the
        # Victor ratio is 1 mickey = 1 pixel, so this lands exactly
        n = 0
        while dx or dy:
            sx = max(-4, min(4, dx))
            sy = max(-4, min(4, dy))
            self.mouse.move(sx, sy)
            dx, dy, n = dx - sx, dy - sy, n + 1
        self._emu_wait(n * 3 * 10 / 1200.0 + 0.3)

    def point(self, col, row):
        """Put the pointer on text cell col,row (cells are 10x16 pixels)."""
        if self.mouse_x is None:
            self._steps(-820, -420)             # into the top-left corner
            self.mouse_x = self.mouse_y = 0
        x, y = col * 10 + 5, row * 16 + 8
        self._steps(x - self.mouse_x, y - self.mouse_y)
        self.mouse_x, self.mouse_y = x, y

    def click(self, col, row, button="L", double=False):
        self.point(col, row)
        for _ in range(2 if double else 1):
            self.mouse.button(button, 1)
            self._emu_wait(0.1)
            self.mouse.button(button, 0)
            self._emu_wait(0.1)
        self._emu_wait(0.3)

    def find(self, text):
        """(col, row) of the first place text is on screen, or None."""
        for row, line in enumerate(self.screen().split("\n")):
            col = line.find(text)
            if col >= 0:
                return col, row
        return None

    def snap(self, name):
        snaps = os.path.join(self.work, "snaps")
        before = set(glob.glob(os.path.join(snaps, "*", "*.png")))
        self.cmd("snap")
        self._wait(lambda: set(glob.glob(os.path.join(snaps, "*", "*.png"))) - before,
                   10, "snapshot")
        new = (set(glob.glob(os.path.join(snaps, "*", "*.png"))) - before).pop()
        dest = os.path.join(snaps, name + ".png")
        os.rename(new, dest)
        return dest


if __name__ == "__main__":
    # Smoke test: boot, start igc from B:, show both panels, quit.
    vm = Victor(sys.argv[1] if len(sys.argv) > 1 else "/srv/prime/work/v9kmame-smoke",
                server_files={"HELLO.TXT": b"hello from igcfs\r\n"})
    try:
        vm.start()
        vm.post("b:\r")
        vm.post("igc\r")
        vm.wait_text("IGC.EXE", timeout=120)
        vm.press("F1")
        print(vm.wait_text("Drive", timeout=30))
        print("snapshot:", vm.snap("igc-f1"))
    finally:
        vm.stop()
