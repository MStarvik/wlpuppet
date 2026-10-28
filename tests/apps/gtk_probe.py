#!/usr/bin/env python3
"""GTK 4 window for exercising wlpuppet: every interaction shows up in a
status line (and on stdout, for tests that read the application log)."""

import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gio, GLib, Gtk  # noqa: E402


class Probe(Gtk.ApplicationWindow):
    def __init__(self, app, confirm_close=False):
        super().__init__(application=app, title="wlpuppet probe")
        if confirm_close:
            self.connect("close-request", self.on_close_request)
        self.set_default_size(480, 360)
        self.clicks = 0

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8, margin_top=12, margin_bottom=12,
                      margin_start=12, margin_end=12)
        self.set_child(box)

        self.status = Gtk.Label(label="ready", xalign=0)
        box.append(self.status)

        row = Gtk.Box(spacing=8)
        box.append(row)
        self.widgets = {}
        button = Gtk.Button(label="Click me")
        self.widgets["button"] = button
        button.connect("clicked", self.on_click)
        row.append(button)

        menu = Gio.Menu()
        menu.append("Alpha", "win.pick::alpha")
        menu.append("Beta", "win.pick::beta")
        action = Gio.SimpleAction.new("pick", GLib.VariantType.new("s"))
        action.connect("activate", lambda _a, value: self.report(f"menu {value.get_string()}"))
        self.add_action(action)
        self.widgets["menu"] = Gtk.MenuButton(label="Menu", menu_model=menu)
        row.append(self.widgets["menu"])

        self.entry = Gtk.Entry(placeholder_text="type here")
        self.entry.connect("changed", lambda e: self.report(f"text {e.get_text()!r}"))
        box.append(self.entry)
        self.widgets["entry"] = self.entry

        rows = Gtk.ListBox()
        for i in range(60):
            rows.append(Gtk.Label(label=f"row {i}", xalign=0))
        scroller = Gtk.ScrolledWindow(vexpand=True)
        scroller.set_child(rows)
        self.widgets["list"] = scroller
        scroller.get_vadjustment().connect("value-changed", lambda adj: self.report(f"scrolled {adj.get_value():.0f}"))
        box.append(scroller)

        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self.on_key)
        self.add_controller(keys)
        self.connect("map", lambda _w: GLib.timeout_add(100, self.print_layout))

    def print_layout(self):
        # Widget bounds in window (geometry) coordinates, so tests can aim.
        for name, widget in self.widgets.items():
            ok, bounds = widget.compute_bounds(self)
            if ok:
                print(f"layout {name} {bounds.origin.x:.0f} {bounds.origin.y:.0f} "
                      f"{bounds.size.width:.0f} {bounds.size.height:.0f}", flush=True)
        return False

    def on_close_request(self, _window):
        # Like an editor with unsaved changes: ask in a separate window.
        dialog = Gtk.Window(title="Really quit?", transient_for=self, modal=True)
        quit_button = Gtk.Button(label="Quit", margin_top=20, margin_bottom=20, margin_start=40, margin_end=40)
        quit_button.connect("clicked", lambda _b: self.get_application().quit())
        dialog.set_child(quit_button)
        dialog.present()
        return True

    def on_click(self, _button):
        self.clicks += 1
        self.report(f"clicked {self.clicks}")

    def on_key(self, _controller, keyval, _keycode, state):
        name = Gtk.accelerator_name(keyval, state)
        print(f"key {name}", flush=True)
        return False

    def report(self, text):
        self.status.set_label(text)
        print(text, flush=True)


confirm_close = "--confirm-close" in sys.argv
app = Gtk.Application(application_id="org.wlpuppet.Probe", flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", lambda a: Probe(a, confirm_close).present())
sys.exit(app.run(None))
