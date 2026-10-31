#!/usr/bin/env python3
"""Run a command while keeping the desktop from going idle (dimming, blanking, locking, suspending).

    inhibit-idle.py APP COMMAND [ARGS...]

On Windows a game controller counts as user activity, so games played with one never ask to keep the display on.
Linux desktops don't see gamepad input, and dim or lock the screen mid-game. This asks every desktop interface that
may be listening, over the session bus, to hold off while COMMAND runs:

  org.freedesktop.portal.Inhibit   the XDG desktop portal: GNOME, KDE and most others, and the one way out of a Flatpak
  org.freedesktop.ScreenSaver      KDE, Xfce, LXQt, Cinnamon, hypridle, xscreensaver-systemd, ...
  org.gnome/org.mate.SessionManager  GNOME and MATE without a portal

Each request lives as long as this process's bus connection, so it ends with COMMAND and also when this process is
killed. Nothing here can stop COMMAND from running: a desktop with none of these, or no session bus at all, just
gets a note on stderr. Standard library only (a minimal D-Bus client), as the AppImage and Flatpak carry no bindings.
"""
import os
import signal
import socket
import struct
import subprocess
import sys


def pad(buf, align):
    buf.extend(b"\0" * (-len(buf) % align))


def marshal(buf, sig, values):
    """Append VALUES with D-Bus signature SIG (only s, o, g, u, v holding s, a{sv}) to BUF, little-endian."""
    i = 0
    for value in values:
        code = sig[i]
        if code in "so":
            data = value.encode()
            pad(buf, 4)
            buf += struct.pack("<I", len(data)) + data + b"\0"
        elif code == "g":
            data = value.encode()
            buf += struct.pack("<B", len(data)) + data + b"\0"
        elif code == "u":
            pad(buf, 4)
            buf += struct.pack("<I", value)
        elif code == "v":
            marshal(buf, "g", ["s"])
            marshal(buf, "s", [value])
        elif sig.startswith("a{sv}", i):
            pad(buf, 4)
            at = len(buf)
            buf += b"\0\0\0\0"
            pad(buf, 8)
            start = len(buf)
            for key, item in value.items():
                pad(buf, 8)
                marshal(buf, "sv", [key, item])
            struct.pack_into("<I", buf, at, len(buf) - start)
            i += 4
        else:
            raise ValueError(sig)
        i += 1


class Bus:
    def __init__(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.address())
        self.sock.sendall(b"\0AUTH EXTERNAL " + str(os.getuid()).encode().hex().encode() + b"\r\n")
        if not self.sock.recv(4096).startswith(b"OK "):
            raise OSError("bus refused authentication")
        self.sock.sendall(b"BEGIN\r\n")
        self.serial = 0
        self.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", "", [])

    @staticmethod
    def address():
        for entry in os.environ.get("DBUS_SESSION_BUS_ADDRESS", "").split(";"):
            kind, _, params = entry.partition(":")
            params = dict(p.split("=", 1) for p in params.split(",") if "=" in p)
            if kind == "unix" and "path" in params:
                return params["path"]
            if kind == "unix" and "abstract" in params:
                return "\0" + params["abstract"]
        return os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid()), "bus")

    def recv_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise OSError("bus closed the connection")
            data += chunk
        return data

    def call(self, dest, path, iface, member, sig, args, autostart=True):
        """Call a method and wait for its reply; raises with the error name if it fails."""
        self.serial += 1
        body = bytearray()
        marshal(body, sig, args)
        fields = [(1, "o", path), (2, "s", iface), (3, "s", member), (6, "s", dest)] + ([(8, "g", sig)] if sig else [])
        header = bytearray(struct.pack("<cBBBII", b"l", 1, 0 if autostart else 2, 1, len(body), self.serial))
        header += b"\0\0\0\0"
        start = len(header)
        for code, kind, value in fields:
            pad(header, 8)
            header += struct.pack("<B", code)
            marshal(header, "g", [kind])
            marshal(header, kind, [value])
        struct.pack_into("<I", header, 12, len(header) - start)
        pad(header, 8)
        self.sock.sendall(bytes(header + body))
        while True:
            fixed = self.recv_exact(16)
            _, kind, _, _, body_len, _, fields_len = struct.unpack("<cBBBIII", fixed)
            rest = self.recv_exact(fields_len + (-(16 + fields_len) % 8) + body_len)
            reply_to, error = self.parse_fields(rest[:fields_len])
            if reply_to != self.serial:
                continue  # a signal (NameAcquired) or anything else not ours
            if kind == 2:  # method return (3 is an error)
                return
            raise OSError(error or "error reply")

    @staticmethod
    def parse_fields(data):
        """REPLY_SERIAL and ERROR_NAME from a header field array."""
        reply_to, error, i = None, None, 0
        while i < len(data):
            i += -i % 8
            code, sig_len = data[i], data[i + 1]
            kind = data[i + 2:i + 2 + sig_len].decode()
            i += 3 + sig_len
            if kind in "so":
                i += -i % 4
                (n,) = struct.unpack_from("<I", data, i)
                value = data[i + 4:i + 4 + n].decode()
                i += 5 + n
            elif kind == "g":
                n = data[i]
                value = data[i + 1:i + 1 + n].decode()
                i += 2 + n
            else:  # u
                i += -i % 4
                (value,) = struct.unpack_from("<I", data, i)
                i += 4
            if code == 5:
                reply_to = value
            elif code == 4:
                error = value
        return reply_to, error


def inhibit(app):
    """Returns the open connection (the requests last as long as it does) and the interfaces that accepted."""
    reason = "%s is running" % app
    requests = [
        # flags: 4 suspend, 8 idle
        ("portal", "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
         "org.freedesktop.portal.Inhibit", "sua{sv}", ["", 4 | 8, {"reason": reason}], True),
        ("screensaver", "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
         "org.freedesktop.ScreenSaver", "ss", ["wine-uwp", reason], False),
        # flags: 4 suspend, 8 idle; 0 = no toplevel window
        ("gnome-session", "org.gnome.SessionManager", "/org/gnome/SessionManager",
         "org.gnome.SessionManager", "susu", ["wine-uwp", 0, reason, 4 | 8], False),
        ("mate-session", "org.mate.SessionManager", "/org/mate/SessionManager",
         "org.mate.SessionManager", "susu", ["wine-uwp", 0, reason, 4 | 8], False),
    ]
    bus = Bus()
    accepted = []
    for name, dest, path, iface, sig, args, autostart in requests:
        try:
            bus.call(dest, path, iface, "Inhibit", sig, args, autostart)
            accepted.append(name)
        except OSError:
            pass
    return bus, accepted


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.split("\n\n")[1])
    app, command = sys.argv[1], sys.argv[2:]
    try:
        bus, accepted = inhibit(app)
        print("wine-uwp: idle inhibited through %s" % (", ".join(accepted) or "nothing (no interface answered)"),
              file=sys.stderr)
    except (OSError, ValueError, IndexError, struct.error) as e:
        bus = None
        print("wine-uwp: could not inhibit idle: %s" % e, file=sys.stderr)

    child = subprocess.Popen(command)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda signum, _: child.send_signal(signum))
    code = child.wait()
    del bus
    sys.exit(code if code >= 0 else 128 - code)


if __name__ == "__main__":
    main()
