#!/usr/bin/env python3
"""The wine-uwp setup wizard: Windows components, the Wine prefix, and the app, one screen at a time.

Run through `wine-uwp setup`, which exports the layout (WINE_UWP_ROOT: the scripts; UWP_BIN: the user's data
directory). Every step is one of the command-line tools, run as a subprocess; the wizard reads their "STAGE",
"PROGRESS" and "RATE" lines and keeps the full output for the details view:

  download   packaging/harvest/download_iso.py     Microsoft's evaluation ISO, resumable and checksummed
  extract    packaging/harvest/harvest.py          only the files wine-uwp needs, into DATA/windows
  prefix     packaging/new-prefix.sh               the Wine prefix, in DATA/prefix
  install    packaging/install-app.py              the user's .msixbundle, into DATA/stage

Each finished step leaves a marker, so a closed or failed setup resumes where it stopped.
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, Gio, GLib, Gtk, Pango  # noqa: E402

APP_ID = os.environ.get("FLATPAK_ID", "io.github.wine_uwp.Setup")
ROOT = os.environ.get("WINE_UWP_ROOT", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
DATA = os.environ.get("UWP_BIN", os.path.join(GLib.get_user_data_dir(), "wine-uwp"))
LAUNCHER = os.path.join(os.environ.get("WINE_BUILD", ""), "wine-uwp")
WINDOWS = os.path.join(DATA, "windows")
PREFIX = os.path.join(DATA, "prefix")
ISO_DIR = os.path.join(DATA, "iso")
ISO = os.path.join(ISO_DIR, "windows11-eval.iso")
ISO_BYTES = 7092807680
PREFIX_READY = os.path.join(PREFIX, ".wine-uwp-ready")
WINDOWS_READY = os.path.join(WINDOWS, ".wine-uwp-harvested")
PACKAGE_FILTERS = ("*.msixbundle", "*.msix", "*.appxbundle", "*.appx")

CSS = b"""
.step-row { padding: 6px 0; }
.step-dim { opacity: 0.45; }
.mono-log { font-family: monospace; font-size: 9pt; }
.hero-icon { -gtk-icon-size: 96px; }
.package-card { padding: 18px; }
"""


def human_bytes(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1000 or unit == "GB":
            return f"{n:.1f} {unit}" if unit in ("GB",) else f"{n:.0f} {unit}"
        n /= 1000
    return f"{n:.1f} GB"


def human_time(s):
    s = int(s)
    if s < 60:
        return f"{s} s"
    if s < 3600:
        return f"{s // 60} min {s % 60:02d} s"
    return f"{s // 3600} h {(s % 3600) // 60:02d} min"


def free_bytes(path):
    while not os.path.exists(path):
        path = os.path.dirname(path)
    return shutil.disk_usage(path).free


def installed_apps():
    stage = os.path.join(DATA, "stage")
    if not os.path.isdir(stage):
        return []
    return sorted(d for d in os.listdir(stage) if os.path.isfile(os.path.join(stage, d, "identity.env")))


def app_title(app):
    try:
        for line in open(os.path.join(DATA, "stage", app, "identity.env")):
            if line.startswith("export WINE_UWP_WINDOW_TITLE="):
                return line.split("=", 1)[1].strip().strip("'")
    except OSError:
        pass
    return app


class Job:
    """One command-line tool, run on a thread; reports lines back on the GTK main loop."""

    def __init__(self, argv, on_line, on_done, env=None):
        self.argv, self.on_line, self.on_done, self.env = argv, on_line, on_done, env
        self.proc = None
        self.cancelled = False

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()

    def cancel(self):
        self.cancelled = True
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def _run(self):
        try:
            self.proc = subprocess.Popen(self.argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                         errors="replace", bufsize=1, env=self.env, start_new_session=True)
        except OSError as e:
            GLib.idle_add(self.on_done, 127, str(e))
            return
        tail = []
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            tail = (tail + [line])[-40:]
            GLib.idle_add(self.on_line, line)
        rc = self.proc.wait()
        reason = next((l for l in reversed(tail) if l and not l.startswith(("PROGRESS", "RATE"))), "")
        GLib.idle_add(self.on_done, -1 if self.cancelled else rc, reason)


class StepList(Gtk.ListBox):
    """The list of steps on a progress page: dim until started, a spinner while running, a tick when done."""

    def __init__(self, steps):
        super().__init__(selection_mode=Gtk.SelectionMode.NONE)
        self.add_css_class("boxed-list")
        self.rows = {}
        for key, title in steps:
            row = Adw.ActionRow(title=title)
            row.add_css_class("step-dim")
            status = Gtk.Stack()
            status.add_named(Gtk.Image.new_from_icon_name("content-loading-symbolic"), "pending")
            status.add_named(Gtk.Spinner(spinning=True), "running")
            status.add_named(Gtk.Image.new_from_icon_name("emblem-ok-symbolic"), "done")
            err = Gtk.Image.new_from_icon_name("dialog-error-symbolic")
            err.add_css_class("error")
            status.add_named(err, "failed")
            status.set_visible_child_name("pending")
            row.add_prefix(status)
            self.append(row)
            self.rows[key] = (row, status)

    def set_state(self, key, state, subtitle=None):
        row, status = self.rows[key]
        status.set_visible_child_name(state)
        if state == "pending":
            row.add_css_class("step-dim")
        else:
            row.remove_css_class("step-dim")
        if subtitle is not None:
            row.set_subtitle(GLib.markup_escape_text(subtitle))

    def reset(self):
        for key in self.rows:
            self.set_state(key, "pending", "")


class ProgressPage(Gtk.Box):
    """A titled progress page: step list, one progress bar, and the tools' full output behind a toggle."""

    def __init__(self, title, description, steps):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=18, margin_top=24, margin_bottom=24,
                         margin_start=12, margin_end=12)
        clamp = Adw.Clamp(maximum_size=560)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        clamp.set_child(box)
        self.append(clamp)
        head = Gtk.Label(label=title, wrap=True, xalign=0)
        head.add_css_class("title-1")
        box.append(head)
        self.description = Gtk.Label(label=description, wrap=True, xalign=0)
        self.description.add_css_class("dim-label")
        box.append(self.description)
        self.steps = StepList(steps)
        box.append(self.steps)
        self.bar = Gtk.ProgressBar(show_text=True)
        box.append(self.bar)
        self.status = Gtk.Label(xalign=0, ellipsize=Pango.EllipsizeMode.END)
        self.status.add_css_class("caption")
        self.status.add_css_class("dim-label")
        box.append(self.status)
        self.log = Gtk.TextView(editable=False, cursor_visible=False, monospace=True, wrap_mode=Gtk.WrapMode.CHAR)
        self.log.add_css_class("mono-log")
        scroller = Gtk.ScrolledWindow(min_content_height=180, child=self.log)
        scroller.add_css_class("card")
        self.expander = Gtk.Expander(label="Details", child=scroller)
        box.append(self.expander)
        self._pulse = 0

    def append_log(self, line):
        buf = self.log.get_buffer()
        buf.insert(buf.get_end_iter(), line + "\n")
        if buf.get_line_count() > 4000:
            buf.delete(buf.get_start_iter(), buf.get_iter_at_line(buf.get_line_count() - 3000)[1])
        self.log.scroll_to_iter(buf.get_end_iter(), 0, False, 0, 0)

    def pulse(self, text=None):
        self.bar.pulse()
        if text is not None:
            self.bar.set_text(text)

    def fraction(self, f, text=None):
        self.bar.set_fraction(max(0.0, min(1.0, f)))
        self.bar.set_text(text if text is not None else f"{int(f * 100)} %")

    def clear(self):
        self.log.get_buffer().set_text("")
        self.steps.reset()
        self.bar.set_fraction(0)
        self.bar.set_text("")
        self.status.set_text("")


class SetupWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="wine-uwp Setup", default_width=720, default_height=720)
        self.job = None
        self.iso_path = None       # an ISO the user picked; None = download
        self.keep_iso = False
        self.package = None        # the package the user picked
        self.package_info = None
        self.installed = None      # stage name of the app just installed
        self.tmp = tempfile.mkdtemp(prefix="wine-uwp-setup-")
        self.stage_times = {}

        view = Adw.ToolbarView()
        header = Adw.HeaderBar()
        view.add_top_bar(header)
        self.toasts = Adw.ToastOverlay()
        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.SLIDE_LEFT_RIGHT, vhomogeneous=False)
        scrolled = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, child=self.stack, vexpand=True)
        self.toasts.set_child(scrolled)
        view.set_content(self.toasts)
        self.bottom = Gtk.Box(spacing=12, halign=Gtk.Align.END, margin_top=12, margin_bottom=12, margin_start=12,
                              margin_end=12)
        view.add_bottom_bar(self.bottom)
        self.back_button = Gtk.Button(label="Back")
        self.next_button = Gtk.Button(label="Continue")
        self.next_button.add_css_class("suggested-action")
        self.next_button.add_css_class("pill")
        self.back_button.add_css_class("pill")
        self.bottom.append(self.back_button)
        self.bottom.append(self.next_button)
        self.back_button.connect("clicked", lambda *_: self.on_back())
        self.next_button.connect("clicked", lambda *_: self.on_next())
        self.set_content(view)
        self.connect("close-request", self.on_close)

        self.build_welcome()
        self.build_source()
        self.prepare = ProgressPage(
            "Preparing Windows components",
            "Downloading Windows 11 from Microsoft and taking out the parts Shapr3D needs. This happens once.",
            [("download", "Download Windows 11 from Microsoft"), ("verify", "Check the download"),
             ("extract", "Extract Windows components"), ("prefix", "Create the Wine environment"),
             ("register", "Register Windows components"), ("symbols", "Fetch debugging symbols from Microsoft"),
             ("components", "Install Windows components")])
        self.stack.add_named(self.prepare, "prepare")
        self.build_package()
        self.install = ProgressPage("Installing", "Unpacking the app and the frameworks it uses.",
                                    [("read", "Read the package"), ("unpack", "Unpack the app"),
                                     ("frameworks", "Install frameworks")])
        self.stack.add_named(self.install, "install")
        self.build_done()
        self.build_error()
        self.start_page()

    # ---- pages ---------------------------------------------------------------------------------------------

    def page(self, name, child):
        clamp = Adw.Clamp(maximum_size=560, margin_top=24, margin_bottom=24, margin_start=12, margin_end=12,
                          child=child)
        self.stack.add_named(clamp, name)

    def build_welcome(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=24)
        status = Adw.StatusPage(icon_name="system-software-install-symbolic", title="Set up Shapr3D on Linux",
                                description="wine-uwp runs the Windows version of Shapr3D. Setting it up takes "
                                            "three steps, and nothing here needs a Windows licence key.")
        box.append(status)
        group = Adw.PreferencesGroup()
        for icon, title, sub in (
                ("folder-download-symbolic", "Windows components",
                 "Microsoft's Windows 11 evaluation image (7.1 GB) is downloaded from Microsoft, and about 2,700 "
                 "files are taken from it."),
                ("applications-system-symbolic", "A Wine environment",
                 "A private Windows environment with Windows' own UI frameworks and fonts, about 3 GB."),
                ("package-x-generic-symbolic", "Shapr3D",
                 "Installed from the Shapr3D for Windows package (.msixbundle) you choose.")):
            row = Adw.ActionRow(title=title, subtitle=sub)
            row.add_prefix(Gtk.Image.new_from_icon_name(icon))
            group.add(row)
        box.append(group)
        self.page("welcome", box)

    def build_source(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        title = Gtk.Label(label="Windows components", xalign=0)
        title.add_css_class("title-1")
        box.append(title)
        intro = Gtk.Label(wrap=True, xalign=0, label=(
            "Shapr3D needs parts of Windows that Wine does not provide: the XAML user interface framework, its "
            "fonts, and the .NET Native and C++ runtimes. They come from Microsoft's own Windows 11 image."))
        intro.add_css_class("dim-label")
        box.append(intro)

        group = Adw.PreferencesGroup()
        self.download_row = Adw.ActionRow(title="Download from Microsoft",
                                          subtitle="Windows 11 Enterprise evaluation, 7.1 GB, from microsoft.com")
        self.download_check = Gtk.CheckButton(active=True)
        self.download_row.add_prefix(self.download_check)
        self.download_row.set_activatable_widget(self.download_check)
        group.add(self.download_row)
        self.iso_row = Adw.ActionRow(title="Use a Windows 11 ISO I already have", subtitle="No file chosen")
        self.iso_check = Gtk.CheckButton(group=self.download_check)
        self.iso_row.add_prefix(self.iso_check)
        choose = Gtk.Button(label="Choose…", valign=Gtk.Align.CENTER)
        choose.connect("clicked", lambda *_: self.choose_iso())
        self.iso_row.add_suffix(choose)
        self.iso_row.set_activatable_widget(self.iso_check)
        group.add(self.iso_row)
        box.append(group)

        opts = Adw.PreferencesGroup()
        self.keep_row = Adw.SwitchRow(title="Keep the downloaded image",
                                      subtitle="Frees 7.1 GB when off; it is only needed to set up again")
        opts.add(self.keep_row)
        box.append(opts)
        self.space_label = Gtk.Label(wrap=True, xalign=0)
        self.space_label.add_css_class("caption")
        box.append(self.space_label)
        note = Gtk.Label(wrap=True, xalign=0, use_markup=True, label=(
            "The image is Microsoft's evaluation release, downloaded straight from Microsoft. Only the files wine-uwp "
            "needs are kept, in your data folder; nothing is installed on your system and nothing is sent anywhere."))
        note.add_css_class("caption")
        note.add_css_class("dim-label")
        box.append(note)
        self.download_check.connect("toggled", lambda *_: self.update_source())
        self.iso_check.connect("toggled", lambda *_: self.update_source())
        self.page("source", box)

    def build_package(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        title = Gtk.Label(label="Choose the Shapr3D package", xalign=0)
        title.add_css_class("title-1")
        box.append(title)
        intro = Gtk.Label(wrap=True, xalign=0, label=(
            "Pick the Shapr3D for Windows package: an .msixbundle (or .msix) file. The frameworks it depends on "
            "come from the Windows components prepared in the last step."))
        intro.add_css_class("dim-label")
        box.append(intro)
        choose = Gtk.Button(label="Choose Package…", halign=Gtk.Align.START)
        choose.add_css_class("pill")
        choose.connect("clicked", lambda *_: self.choose_package())
        box.append(choose)
        self.card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, visible=False)
        self.card.add_css_class("card")
        self.card.add_css_class("package-card")
        top = Gtk.Box(spacing=18)
        self.card_icon = Gtk.Image(pixel_size=64, icon_name="package-x-generic-symbolic")
        top.append(self.card_icon)
        names = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4, valign=Gtk.Align.CENTER)
        self.card_title = Gtk.Label(xalign=0)
        self.card_title.add_css_class("title-2")
        self.card_sub = Gtk.Label(xalign=0, wrap=True)
        self.card_sub.add_css_class("dim-label")
        names.append(self.card_title)
        names.append(self.card_sub)
        top.append(names)
        self.card.append(top)
        self.card_deps = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.card_deps.add_css_class("boxed-list")
        self.card.append(self.card_deps)
        box.append(self.card)
        self.package_error = Gtk.Label(wrap=True, xalign=0, visible=False)
        self.package_error.add_css_class("error")
        box.append(self.package_error)
        self.page("package", box)

    def build_done(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self.done_status = Adw.StatusPage(title="Shapr3D is ready", icon_name="emblem-ok-symbolic")
        box.append(self.done_status)
        group = Adw.PreferencesGroup()
        self.menu_row = Adw.SwitchRow(title="Add to the applications menu", active=True,
                                      subtitle="Also lets your browser hand sign-ins back to the app")
        group.add(self.menu_row)
        self.done_group = group
        box.append(group)
        hint = Gtk.Label(wrap=True, xalign=0, label=(
            "Signing in opens your web browser. When it asks to open the link with wine-uwp, allow it: that is how "
            "the sign-in gets back to Shapr3D."))
        hint.add_css_class("dim-label")
        hint.add_css_class("caption")
        box.append(hint)
        self.page("done", box)

    def build_error(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self.error_status = Adw.StatusPage(icon_name="dialog-error-symbolic", title="Setup stopped")
        box.append(self.error_status)
        self.error_log = Gtk.TextView(editable=False, monospace=True, wrap_mode=Gtk.WrapMode.CHAR)
        self.error_log.add_css_class("mono-log")
        scroller = Gtk.ScrolledWindow(min_content_height=200, child=self.error_log)
        scroller.add_css_class("card")
        box.append(Gtk.Expander(label="Details", child=scroller))
        self.page("error", box)

    # ---- navigation ----------------------------------------------------------------------------------------

    def show(self, name):
        self.current = name
        self.stack.set_visible_child_name(name)
        buttons = {
            "welcome": (None, "Get Started"),
            "source": ("Back", "Continue"),
            "prepare": (None, "Cancel"),
            "package": ("Back" if not os.path.exists(PREFIX_READY) else None, "Install"),
            "install": (None, "Cancel"),
            "done": ("Close", f"Launch {self.done_title()}"),
            "error": ("Close", "Try Again"),
        }[name]
        self.back_button.set_visible(buttons[0] is not None)
        if buttons[0]:
            self.back_button.set_label(buttons[0])
        self.next_button.set_label(buttons[1])
        cancel = name in ("prepare", "install")
        self.next_button.remove_css_class("suggested-action" if cancel else "destructive-action")
        self.next_button.add_css_class("destructive-action" if cancel else "suggested-action")
        if name == "source":
            self.update_source()
        if name == "package":
            self.next_button.set_sensitive(self.package_info is not None and self.package_ok())

    def start_page(self):
        if os.path.exists(WINDOWS_READY) and os.path.exists(PREFIX_READY):
            apps = installed_apps()
            if apps:
                self.installed = apps[0]
                self.fill_done()
                self.show("done")
            else:
                self.show("package")
        else:
            self.show("welcome")

    def on_back(self):
        if self.current in ("done", "error"):
            self.close()
        elif self.current == "source":
            self.show("welcome")
        elif self.current == "package":
            self.show("source")

    def on_next(self):
        if self.current == "welcome":
            self.show("source")
        elif self.current == "source":
            self.keep_iso = self.keep_row.get_active()
            self.run_prepare()
        elif self.current in ("prepare", "install"):
            self.confirm_cancel()
        elif self.current == "package":
            self.run_install()
        elif self.current == "done":
            self.launch()
        elif self.current == "error":
            self.retry()

    def on_close(self, *_):
        if self.job and self.current in ("prepare", "install"):
            self.confirm_cancel(close_after=True)
            return True
        shutil.rmtree(self.tmp, ignore_errors=True)
        return False

    def confirm_cancel(self, close_after=False):
        dialog = Adw.AlertDialog(heading="Stop the setup?",
                                 body="What is finished is kept; the setup continues from here next time.")
        dialog.add_response("continue", "Keep Going")
        dialog.add_response("stop", "Stop")
        dialog.set_response_appearance("stop", Adw.ResponseAppearance.DESTRUCTIVE)

        def done(d, result):
            if d.choose_finish(result) == "stop" and self.job:
                self.job.cancel()
                self.close_after_cancel = close_after
        dialog.choose(self, None, done)

    # ---- source page ---------------------------------------------------------------------------------------

    def update_source(self):
        downloading = self.download_check.get_active()
        self.keep_row.set_sensitive(downloading)
        part = ISO + ".part"
        have = os.path.getsize(ISO) if os.path.exists(ISO) else os.path.getsize(part) if os.path.exists(part) else 0
        need = (ISO_BYTES - have if downloading else 0) + 4_000_000_000
        free = free_bytes(DATA)
        text = f"Needs about {human_bytes(need)} free in {DATA}; {human_bytes(free)} available."
        self.space_label.set_text(text)
        self.space_label.remove_css_class("error")
        ok = free >= need
        if not ok:
            self.space_label.add_css_class("error")
        self.next_button.set_sensitive(ok and (downloading or self.iso_path is not None))

    def choose_iso(self):
        dialog = Gtk.FileDialog(title="Choose a Windows 11 ISO")
        filt = Gtk.FileFilter(name="Disc images (*.iso)")
        filt.add_pattern("*.iso")
        filt.add_pattern("*.ISO")
        store = Gio.ListStore.new(Gtk.FileFilter)
        store.append(filt)
        dialog.set_filters(store)

        def done(d, result):
            try:
                f = d.open_finish(result)
            except GLib.Error:
                return
            self.iso_path = f.get_path()
            self.iso_row.set_subtitle(GLib.markup_escape_text(os.path.basename(self.iso_path)))
            self.iso_check.set_active(True)
            self.update_source()
        dialog.open(self, None, done)

    # ---- running the tools ---------------------------------------------------------------------------------

    def run_job(self, argv, page, on_line, on_done):
        page.append_log("$ " + " ".join(argv))
        self.job = Job(argv, lambda line: (page.append_log(line), on_line(line)), on_done)
        self.job.start()

    def fail(self, title, message, page):
        self.job = None
        if getattr(self, "close_after_cancel", False):
            self.close()
            return
        self.error_status.set_title(title)
        self.error_status.set_description(message)
        buf = page.log.get_buffer()
        self.error_log.get_buffer().set_text(buf.get_text(buf.get_start_iter(), buf.get_end_iter(), False))
        self.retry = self.retry_prepare if page is self.prepare else self.retry_install
        self.show("error")

    def retry_prepare(self):
        self.run_prepare()

    def retry_install(self):
        self.show("package")

    def run_prepare(self):
        self.prepare.clear()
        self.show("prepare")
        if not self.download_check.get_active():
            self.prepare.steps.set_state("download", "done", "Using " + os.path.basename(self.iso_path))
            self.prepare.steps.set_state("verify", "done", "Checked by the extraction")
            self.after_download(self.iso_path)
        elif os.path.exists(ISO) and os.path.exists(ISO + ".verified"):
            self.prepare.steps.set_state("download", "done", "Already downloaded")
            self.prepare.steps.set_state("verify", "done")
            self.after_download(ISO)
        else:
            self.run_download()

    def run_download(self):
        os.makedirs(ISO_DIR, exist_ok=True)
        steps = self.prepare.steps
        steps.set_state("download", "running", "Contacting Microsoft…")
        state = {"rate": 0}

        def line(l):
            if l.startswith("PROGRESS "):
                done, total = (int(x) for x in l.split()[1:3])
                if total:
                    left = (total - done) / state["rate"] if state["rate"] else 0
                    text = f"{human_bytes(done)} of {human_bytes(total)}"
                    if state["rate"]:
                        text += f" · {human_bytes(state['rate'])}/s · {human_time(left)} left"
                    self.prepare.fraction(done / total, text)
                    steps.set_state("download", "running", text)
            elif l.startswith("RATE "):
                state["rate"] = int(l.split()[1])
            elif l.startswith("STAGE Verifying"):
                steps.set_state("download", "done", "Downloaded from microsoft.com")
                steps.set_state("verify", "running", "Checking the SHA-256 checksum")
                self.prepare.pulse("Checking the download…")
            elif l.startswith("VERIFIED "):
                steps.set_state("verify", "done", l[9:])
            elif l.startswith("UNVERIFIED "):
                steps.set_state("verify", "done", "A newer image than this setup was tested with")

        def done(rc, reason):
            self.job = None
            if rc != 0:
                steps.set_state("download", "failed")
                self.fail("The download stopped" if rc > 0 else "Setup stopped",
                          (reason or "The download did not finish.") + " Running the setup again continues it.",
                          self.prepare)
                return
            open(ISO + ".verified", "w").close()
            self.after_download(ISO)

        self.run_job([sys.executable, os.path.join(ROOT, "packaging", "harvest", "download_iso.py"), ISO],
                     self.prepare, line, done)

    def after_download(self, iso):
        if os.path.exists(WINDOWS_READY):
            self.prepare.steps.set_state("extract", "done", "Already extracted")
            self.run_prefix()
            return
        steps = self.prepare.steps
        steps.set_state("extract", "running", "Opening the image…")
        partial = WINDOWS + ".partial"
        shutil.rmtree(partial, ignore_errors=True)

        def line(l):
            if l.startswith("PROGRESS "):
                done, total = (int(x) for x in l.split()[1:3])
                self.prepare.fraction(done / max(total, 1), f"Extracting… {done} %")
            elif l.startswith("STAGE "):
                steps.set_state("extract", "running", l[6:])
                if not l.startswith("STAGE Extracting"):
                    self.prepare.pulse(l[6:])

        def done(rc, reason):
            self.job = None
            if rc != 0:
                steps.set_state("extract", "failed")
                self.fail("Could not extract the Windows components",
                          reason or "The image could not be read.", self.prepare)
                return
            shutil.rmtree(WINDOWS, ignore_errors=True)
            os.rename(partial, WINDOWS)
            open(WINDOWS_READY, "w").close()
            missing = os.path.join(WINDOWS, "harvest-missing.txt")
            n = sum(1 for _ in open(missing)) if os.path.exists(missing) else 0
            steps.set_state("extract", "done", "Done" + (f"; {n} optional files are not in this image" if n else ""))
            if iso == ISO and not self.keep_iso:
                for f in (ISO, ISO + ".verified"):
                    if os.path.exists(f):
                        os.remove(f)
            self.run_prefix()

        self.run_job([sys.executable, os.path.join(ROOT, "packaging", "harvest", "harvest.py"), iso, partial],
                     self.prepare, line, done)

    def run_prefix(self):
        steps = self.prepare.steps
        if os.path.exists(PREFIX_READY):
            for k in ("prefix", "register", "symbols", "components"):
                steps.set_state(k, "done")
            self.show_package()
            return
        shutil.rmtree(PREFIX, ignore_errors=True)
        order = ["prefix", "register", "symbols", "components"]
        stage_of = [("Creating the Wine prefix", "prefix"), ("Registering", "register"),
                    ("Resolving Windows symbols", "symbols"), ("Fetching symbols", "symbols"),
                    ("Installing Windows components", "components")]
        state = {"current": None}
        timer = GLib.timeout_add(120, lambda: (self.prepare.pulse(), True)[1])

        def enter(key, sub):
            if state["current"] and state["current"] != key:
                for k in order[:order.index(key)]:
                    steps.set_state(k, "done", "")
            state["current"] = key
            steps.set_state(key, "running", sub)

        enter("prefix", "Starting Wine")
        self.prepare.pulse("Setting up Wine…")

        def line(l):
            if l.startswith("STAGE "):
                what = l[6:]
                for prefix, key in stage_of:
                    if what.startswith(prefix):
                        enter(key, what)
                        break
                self.prepare.bar.set_text(what)
            elif l.startswith(("installed:", "registered", "verified:")):
                self.prepare.status.set_text(l)

        def done(rc, reason):
            self.job = None
            GLib.source_remove(timer)
            if rc != 0:
                steps.set_state(state["current"], "failed")
                self.fail("Could not create the Wine environment", reason or "new-prefix.sh failed.", self.prepare)
                return
            for k in order:
                steps.set_state(k, "done", "")
            open(PREFIX_READY, "w").close()
            self.prepare.fraction(1, "Done")
            GLib.timeout_add(600, lambda: (self.show_package(), False)[1])

        env = dict(os.environ, WINEDEBUG="-all")
        self.job = Job(["bash", os.path.join(ROOT, "packaging", "new-prefix.sh"), WINDOWS, PREFIX],
                       lambda l: (self.prepare.append_log(l), line(l)), done, env=env)
        self.prepare.append_log("$ new-prefix.sh " + WINDOWS + " " + PREFIX)
        self.job.start()

    # ---- package page --------------------------------------------------------------------------------------

    def show_package(self):
        self.show("package")

    def choose_package(self):
        dialog = Gtk.FileDialog(title="Choose the Shapr3D package")
        filt = Gtk.FileFilter(name="App packages (.msixbundle, .msix)")
        for p in PACKAGE_FILTERS:
            filt.add_pattern(p)
        store = Gio.ListStore.new(Gtk.FileFilter)
        store.append(filt)
        dialog.set_filters(store)

        def done(d, result):
            try:
                f = d.open_finish(result)
            except GLib.Error:
                return
            self.load_package(f.get_path())
        dialog.open(self, None, done)

    def load_package(self, path):
        icon = os.path.join(self.tmp, "icon.png")
        if os.path.exists(icon):
            os.remove(icon)
        r = subprocess.run([sys.executable, os.path.join(ROOT, "packaging", "install-app.py"), path, "--info",
                            "--icon", icon, "--windows", WINDOWS], capture_output=True, text=True)
        self.package_error.set_visible(r.returncode != 0)
        if r.returncode != 0:
            self.package_info = None
            self.card.set_visible(False)
            self.package_error.set_text((r.stderr or r.stdout).strip().splitlines()[-1] if (r.stderr or r.stdout)
                                        else "This file is not an app package.")
            self.next_button.set_sensitive(False)
            return
        self.package = path
        self.package_info = info = json.loads(r.stdout)
        self.card_title.set_text(info["title"])
        self.card_sub.set_text(f"Version {info['version']} · {info['publisher'] or info['name']} · "
                               f"{info['architecture']}")
        if info.get("icon") and os.path.exists(info["icon"]):
            self.card_icon.set_from_file(info["icon"])
        else:
            self.card_icon.set_from_icon_name("package-x-generic-symbolic")
        while (row := self.card_deps.get_first_child()) is not None:
            self.card_deps.remove(row)
        for fw in info["frameworks"]:
            row = Adw.ActionRow(title=fw["name"], subtitle=f"{fw['min_version']} or later")
            ok = fw["available"]
            img = Gtk.Image.new_from_icon_name("emblem-ok-symbolic" if ok else "dialog-error-symbolic")
            img.add_css_class("success" if ok else "error")
            row.add_suffix(img)
            self.card_deps.append(row)
        self.card.set_visible(True)
        if not self.package_ok():
            self.package_error.set_text("Some frameworks this package needs are not in the Windows components.")
            self.package_error.set_visible(True)
        if info["architecture"].lower() not in ("x64", "neutral"):
            self.package_error.set_text(f"This package is for {info['architecture']}; wine-uwp needs x64.")
            self.package_error.set_visible(True)
        self.next_button.set_sensitive(self.package_ok())

    def package_ok(self):
        info = self.package_info
        return bool(info) and all(fw["available"] for fw in info["frameworks"]) and \
            info["architecture"].lower() in ("x64", "neutral")

    # ---- installing ----------------------------------------------------------------------------------------

    def run_install(self):
        self.install.clear()
        self.show("install")
        steps = self.install.steps
        steps.set_state("read", "running")

        def line(l):
            if l.startswith("STAGE Reading"):
                steps.set_state("read", "running")
            elif l.startswith("STAGE Unpacking"):
                steps.set_state("read", "done")
                steps.set_state("unpack", "running", l[6:])
            elif l.startswith("PROGRESS "):
                done, total = (int(x) for x in l.split()[1:3])
                self.install.fraction(done / max(total, 1))
            elif l.startswith("STAGE Installing frameworks"):
                steps.set_state("unpack", "done")
                steps.set_state("frameworks", "running")
            elif l.startswith("framework "):
                steps.set_state("frameworks", "running", l[10:])
            elif l.startswith("INSTALLED "):
                self.installed = l.split()[1]

        def done(rc, reason):
            self.job = None
            if rc != 0 or not self.installed:
                for k in ("read", "unpack", "frameworks"):
                    row, status = steps.rows[k]
                    if status.get_visible_child_name() == "running":
                        steps.set_state(k, "failed")
                self.fail("Could not install the package", reason or "install-app.py failed.", self.install)
                return
            for k in ("read", "unpack", "frameworks"):
                steps.set_state(k, "done")
            self.install.fraction(1, "Installed")
            self.fill_done()
            GLib.timeout_add(500, lambda: (self.show("done"), False)[1])

        self.installed = None
        self.run_job([sys.executable, os.path.join(ROOT, "packaging", "install-app.py"), self.package, DATA,
                      "--windows", WINDOWS], self.install, line, done)

    # ---- done ----------------------------------------------------------------------------------------------

    def done_title(self):
        return app_title(self.installed) if self.installed else "App"

    def fill_done(self):
        title = self.done_title()
        self.done_status.set_title(f"{title} is ready")
        self.done_status.set_description("Setup is complete. You can start it from here or from your applications "
                                         "menu.")
        icon = os.path.join(DATA, "stage", self.installed, ".wine-uwp-icon.png")
        if os.path.exists(icon):
            self.done_status.set_paintable(Gtk.Image.new_from_file(icon).get_paintable())
        # inside a Flatpak the app's own desktop file and scheme handler are exported by the Flatpak itself
        self.done_group.set_visible("FLATPAK_ID" not in os.environ)

    def write_desktop_entry(self):
        app = self.installed
        title = self.done_title()
        apps = os.path.join(GLib.get_user_data_dir(), "applications")
        os.makedirs(apps, exist_ok=True)
        stage = os.path.join(DATA, "stage", app)
        schemes = []
        pfile = os.path.join(stage, ".wine-uwp-protocols")
        if os.path.exists(pfile):
            schemes = [l.strip() for l in open(pfile) if l.strip()]
        icon = os.path.join(stage, ".wine-uwp-icon.png")
        entry = [
            "[Desktop Entry]", "Type=Application", f"Name={title}", "Comment=Runs with wine-uwp",
            f"Exec={LAUNCHER} run {app}", f"Icon={icon if os.path.exists(icon) else 'application-x-executable'}",
            "Categories=Graphics;Engineering;", f"StartupWMClass={title}",
        ]
        path = os.path.join(apps, f"wine-uwp-{app}.desktop")
        with open(path, "w") as f:
            f.write("\n".join(entry) + "\n")
        if schemes:
            handler = os.path.join(apps, f"wine-uwp-{app}-handler.desktop")
            with open(handler, "w") as f:
                f.write("\n".join([
                    "[Desktop Entry]", "Type=Application", f"Name={title} sign-in", "NoDisplay=true",
                    f"Exec={LAUNCHER} activate %u",
                    "MimeType=" + "".join(f"x-scheme-handler/{s};" for s in schemes)]) + "\n")
            for s in schemes:
                subprocess.run(["xdg-mime", "default", os.path.basename(handler), f"x-scheme-handler/{s}"],
                               capture_output=True)
        subprocess.run(["update-desktop-database", apps], capture_output=True)

    def launch(self):
        if self.menu_row.get_active() and self.done_group.get_visible():
            try:
                self.write_desktop_entry()
            except OSError as e:
                self.toasts.add_toast(Adw.Toast(title=f"Could not add the menu entry: {e}"))
        subprocess.Popen([LAUNCHER, "run", self.installed], start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.next_button.set_sensitive(False)
        self.next_button.set_label("Starting…")
        GLib.timeout_add_seconds(4, lambda: (self.close(), False)[1])


class SetupApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id=APP_ID, flags=Gio.ApplicationFlags.NON_UNIQUE)

    def do_activate(self):
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), provider,
                                                  Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        SetupWindow(self).present()


if __name__ == "__main__":
    sys.exit(SetupApp().run(sys.argv[:1]))
