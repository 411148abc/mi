"""Tkinter shell for Mi Browser — the Python edition.

    python -m 米.浏览器                       # open the browser window
    python -m 米.浏览器 --open URL            # open an MI app over HTTP
    python -m 米.浏览器 --source FILE.mi      # open a local MI program
    python -m 米.浏览器 --selftest            # headless conformance self-test
"""
from __future__ import annotations

import argparse
import os
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk

from ..errors import render_runtime_error
from ..glyphs import TABLE
from .. import __version__
from .conformance import run_selftest
from .controller import BrowserController

BG = "#0d1117"
PANEL = "#161b22"
PANEL2 = "#1f2733"
TEXT = "#e6edf3"
MUTED = "#8b949e"
ACCENT = "#3fb950"
ACCENT_DARK = "#0b2e16"
BLUE = "#58a6ff"
ERROR = "#f85149"
WARN = "#d29922"
INPUT_BG = "#0b0f16"
BORDER = "#30363d"

ROOT = Path(__file__).resolve().parents[2]
MONO = ("Consolas", 11)
MONO_SMALL = ("Consolas", 10)

DEV_HINTS = "Ctrl+Enter 运行 · Ctrl+L 地址栏 · Ctrl+O 打开文件 · F11 应用模式"
APP_HINTS = "F11 或点“开发模式”返回编辑器 · Ctrl+L 地址栏"

# Window title.  Opening an app replaces it with that app's own title, so the
# screenshot of a running system shows the system name, not the browser name.
DEFAULT_TITLE = "Mi Browser"

WELCOME = """-- Mi Browser · Python edition
-- The interpreter under this window is the MI reference implementation in Python.
-- App address bar: try  http://127.0.0.1:8080/app/forum.mi  (start the forum backend first).

station greet(event) {
    ~name: ui.value(label: "your name")
    ui.text(value: "hello, " + name + " — this page is running on Python, not JavaScript.")
}

ui.mount()
ui.heading(value: "米 Mi Browser")
ui.text(value: "Same MI language, same ui.* host program, but executed by the Python runtime.")
ui.input(label: "your name", value: "mi")
ui.button(label: "greet", press: greet)
"""


def _discover_examples() -> list[str]:
    found: list[str] = []
    examples = ROOT / "示例"
    if examples.is_dir():
        found.extend(
            str(path.relative_to(ROOT)).replace("\\", "/")
            for path in sorted(examples.rglob("*.mi"))
        )
    apps = ROOT / "应用"
    if apps.is_dir():
        found.extend(
            str(path.relative_to(ROOT)).replace("\\", "/")
            for path in sorted(apps.rglob("*.mi"))
            if path.name != "backend.mi"
        )
    return found


class MiBrowserApp:
    def __init__(
        self,
        root: tk.Tk,
        *,
        open_url: str | None = None,
        source_path: str | None = None,
        size: str = "1440x900",
    ) -> None:
        self.root = root
        self.controller = BrowserController(
            on_change=self.refresh,
            max_steps=5_000_000,
        )
        self._refreshing = False
        self._input_vars: dict[str, tk.StringVar] = {}
        self._containers: dict[int, tk.Frame] = {}
        self._timer_job: str | None = None
        self._resize_job: str | None = None
        self._examples = _discover_examples()
        self._app_mode = False
        self._auto_app_mode = bool(source_path or open_url)
        self._user_switched = False
        self._fullscreen = False
        self._windowed_geometry = size + "+20+20"

        self.root.title(DEFAULT_TITLE)
        self.root.geometry(size)
        self.root.minsize(1024, 680)
        self.root.configure(bg=BG)
        self._style()
        self._topbar()
        self._main_area()
        self._statusbar()
        self._bind_keys()
        self._render_reference()

        if source_path:
            path = Path(source_path)
            try:
                self.load_source(path.read_text(encoding="utf-8-sig"), str(path))
                try:
                    shown = path.resolve().relative_to(ROOT).as_posix()
                except ValueError:
                    shown = str(path)
                self.address_var.set(f"local:{shown}")
            except OSError as exc:
                self._flash_error(f"cannot read {path}: {exc}")
        elif open_url:
            self.address_var.set(open_url)
            self.root.after(120, lambda: self._open_url(open_url))
        else:
            self.editor.insert("1.0", WELCOME)
            self._run_editor()
        # opening an app starts in 应用模式 (the ui pane fills the window)
        if self._auto_app_mode:
            self.root.after(200, self._maybe_enter_app_mode)

    # ------------------------------------------------------------------ style
    def _style(self) -> None:
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:  # pragma: no cover - defensive
            pass
        style.configure("TNotebook", background=BG, borderwidth=0, tabmargins=(0, 0, 0, 0))
        style.configure(
            "TNotebook.Tab",
            background=PANEL,
            foreground=MUTED,
            padding=(16, 8),
            borderwidth=0,
            font=("Segoe UI", 10),
        )
        style.map(
            "TNotebook.Tab",
            background=[("selected", PANEL2)],
            foreground=[("selected", ACCENT)],
        )
        style.configure("TPanedwindow", background=BG)
        # in 应用模式 the tab strip disappears so the rendered page owns the window
        style.configure("AppMode.TNotebook", background=PANEL, borderwidth=0, tabmargins=(0, 0, 0, 0))
        style.layout("AppMode.TNotebook", [("Notebook.client", {"sticky": "nswe"})])
        style.configure(
            "Vertical.TScrollbar",
            background=PANEL,
            troughcolor=BG,
            bordercolor=BG,
            arrowcolor=MUTED,
        )
        style.configure("TCombobox", fieldbackground=INPUT_BG, background=PANEL, foreground=TEXT)

    # ---------------------------------------------------------------- layout
    def _label(self, parent, text: str, *, fg=TEXT, bg=BG, font=MONO_SMALL, **kwargs):
        return tk.Label(parent, text=text, fg=fg, bg=bg, font=font, **kwargs)

    def _button(self, parent, text: str, command, *, primary=False):
        return tk.Button(
            parent,
            text=text,
            command=command,
            bg=ACCENT if primary else PANEL2,
            fg=ACCENT_DARK if primary else TEXT,
            activebackground="#56d364" if primary else "#2a3442",
            activeforeground=ACCENT_DARK if primary else TEXT,
            relief="flat",
            bd=0,
            padx=14,
            pady=5,
            cursor="hand2",
            font=MONO_SMALL,
        )

    def _topbar(self) -> None:
        bar = tk.Frame(self.root, bg=PANEL, height=52)
        bar.pack(side="top", fill="x")
        bar.pack_propagate(False)
        self.topbar = bar

        self.address_var = tk.StringVar(value="http://127.0.0.1:8080/app/forum.mi")
        self.address_entry = tk.Entry(
            bar,
            textvariable=self.address_var,
            bg=INPUT_BG,
            fg=TEXT,
            insertbackground=TEXT,
            relief="flat",
            bd=0,
            highlightthickness=1,
            highlightbackground=BORDER,
            highlightcolor=BLUE,
            font=MONO_SMALL,
        )
        self.address_entry.pack(side="left", fill="x", expand=True, padx=(12, 8), ipady=6)
        self.address_entry.bind("<Return>", lambda _event: self._open_url())

        self.open_button = self._button(bar, "Open app", self._open_url)
        self.open_button.pack(side="left", padx=(0, 6), ipady=2)
        self.run_button = self._button(bar, "Run  Ctrl+↵", self._run_editor, primary=True)
        self.run_button.pack(side="left", padx=(0, 6), ipady=2)
        # F11-style switch between 应用模式 (ui fills the window) and 开发模式
        self.mode_button = self._button(bar, "应用模式", self.toggle_app_mode)
        self.mode_button.pack(side="left", padx=(0, 6), ipady=2)
        self.fullscreen_button = self._button(bar, "全屏", self.toggle_fullscreen)
        self.fullscreen_button.pack(side="left", padx=(0, 10), ipady=2)

        self.status_var = tk.StringVar(value="ready")
        self.status_label = tk.Label(
            bar, textvariable=self.status_var, fg=MUTED, bg=PANEL, font=MONO_SMALL
        )
        self.status_label.pack(side="right", padx=(0, 14))

    def _main_area(self) -> None:
        paned = ttk.Panedwindow(self.root, orient="horizontal")
        paned.pack(side="top", fill="both", expand=True, padx=10, pady=(8, 6))
        self.paned = paned
        self.editor_pane = paned

        left = tk.Frame(paned, bg=PANEL)
        paned.add(left, weight=5)
        self.left_pane = left
        self._label(left, "EDITOR", fg=MUTED, bg=PANEL).pack(anchor="w", padx=10, pady=(8, 4))

        editor_frame = tk.Frame(left, bg=PANEL)
        editor_frame.pack(fill="both", expand=True, padx=10)
        self.editor = tk.Text(
            editor_frame,
            bg=INPUT_BG,
            fg=TEXT,
            insertbackground=TEXT,
            selectbackground="#264f78",
            relief="flat",
            bd=0,
            wrap="none",
            undo=True,
            font=MONO,
            padx=10,
            pady=8,
        )
        editor_scroll = ttk.Scrollbar(editor_frame, orient="vertical", command=self.editor.yview)
        self.editor.configure(yscrollcommand=editor_scroll.set)
        self.editor.pack(side="left", fill="both", expand=True)
        editor_scroll.pack(side="right", fill="y")

        self._label(left, "DIAGNOSTICS", fg=MUTED, bg=PANEL).pack(
            anchor="w", padx=10, pady=(8, 4)
        )
        diag_frame = tk.Frame(left, bg=PANEL)
        diag_frame.pack(fill="x", padx=10, pady=(0, 10))
        self.diagnostics = tk.Text(
            diag_frame,
            height=8,
            bg=INPUT_BG,
            fg=TEXT,
            insertbackground=TEXT,
            relief="flat",
            bd=0,
            wrap="word",
            font=MONO_SMALL,
            padx=8,
            pady=6,
            state="disabled",
        )
        self.diagnostics.pack(side="left", fill="both", expand=True)
        diag_scroll = ttk.Scrollbar(
            diag_frame, orient="vertical", command=self.diagnostics.yview
        )
        self.diagnostics.configure(yscrollcommand=diag_scroll.set)
        diag_scroll.pack(side="right", fill="y")

        right = tk.Frame(paned, bg=BG)
        paned.add(right, weight=6)
        self.right_pane = right
        self.notebook = ttk.Notebook(right)
        self.notebook.pack(fill="both", expand=True)

        # console tab
        console_frame = tk.Frame(self.notebook, bg=PANEL)
        self.console = tk.Text(
            console_frame,
            bg=PANEL,
            fg=TEXT,
            relief="flat",
            bd=0,
            wrap="word",
            font=MONO_SMALL,
            padx=12,
            pady=10,
            state="disabled",
        )
        console_scroll = ttk.Scrollbar(
            console_frame, orient="vertical", command=self.console.yview
        )
        self.console.configure(yscrollcommand=console_scroll.set)
        self.console.pack(side="left", fill="both", expand=True)
        console_scroll.pack(side="right", fill="y")
        self.notebook.add(console_frame, text="console")

        # ui tab — the rendered page lives in `ui_host`, placed inside this tab in
        # 开发模式 and directly under the toolbar in 应用模式 (Tk's `pack -in`
        # lets one widget move between containers without being recreated).
        ui_frame = tk.Frame(self.notebook, bg=PANEL)
        self.ui_tab = ui_frame
        self.ui_host = tk.Frame(self.root, bg=PANEL)
        self.ui_canvas = tk.Canvas(
            self.ui_host, bg=PANEL, bd=0, highlightthickness=0
        )
        ui_scroll = ttk.Scrollbar(
            self.ui_host, orient="vertical", command=self.ui_canvas.yview
        )
        self.ui_canvas.configure(yscrollcommand=ui_scroll.set)
        self.ui_canvas.pack(side="left", fill="both", expand=True)
        ui_scroll.pack(side="right", fill="y")
        self.ui_inner = tk.Frame(self.ui_canvas, bg=PANEL)
        self._ui_window = self.ui_canvas.create_window(
            (0, 0), window=self.ui_inner, anchor="nw"
        )
        self.ui_inner.bind(
            "<Configure>",
            lambda _event: self.ui_canvas.configure(
                scrollregion=self.ui_canvas.bbox("all")
            ),
        )
        self.ui_canvas.bind(
            "<Configure>",
            lambda event: self.ui_canvas.itemconfigure(
                self._ui_window, width=max(320, event.width)
            ),
        )
        # mouse wheel scrolls the rendered app (bound on hover, not globally)
        self.ui_canvas.bind("<Enter>", lambda _event: self._bind_wheel(True))
        self.ui_canvas.bind("<Leave>", lambda _event: self._bind_wheel(False))
        self.ui_host.pack(in_=ui_frame, fill="both", expand=True)
        self.notebook.add(ui_frame, text="ui")

        # reference tab
        reference_frame = tk.Frame(self.notebook, bg=PANEL)
        self.reference = tk.Text(
            reference_frame,
            bg=PANEL,
            fg=TEXT,
            relief="flat",
            bd=0,
            wrap="word",
            font=MONO_SMALL,
            padx=12,
            pady=10,
            state="disabled",
        )
        reference_scroll = ttk.Scrollbar(
            reference_frame, orient="vertical", command=self.reference.yview
        )
        self.reference.configure(yscrollcommand=reference_scroll.set)
        self.reference.pack(side="left", fill="both", expand=True)
        reference_scroll.pack(side="right", fill="y")
        self.notebook.add(reference_frame, text="reference")

        # self-test tab
        test_frame = tk.Frame(self.notebook, bg=PANEL)
        tools = tk.Frame(test_frame, bg=PANEL)
        tools.pack(fill="x", padx=12, pady=10)
        self._button(tools, "Run all conformance cases", self._run_selftest, primary=True).pack(
            side="left"
        )
        self.selftest_summary = tk.StringVar(value="not run yet")
        tk.Label(
            tools, textvariable=self.selftest_summary, fg=MUTED, bg=PANEL, font=MONO_SMALL
        ).pack(side="left", padx=12)
        self.selftest_text = tk.Text(
            test_frame,
            bg=PANEL,
            fg=TEXT,
            relief="flat",
            bd=0,
            wrap="word",
            font=MONO_SMALL,
            padx=12,
            pady=4,
            state="disabled",
        )
        self.selftest_text.pack(fill="both", expand=True)
        self.notebook.add(test_frame, text="self-test")

        self.tab_console = console_frame
        self.tab_ui = ui_frame
        self.tab_reference = reference_frame
        self.tab_selftest = test_frame

    # ------------------------------------------------------------- app mode
    def _maybe_enter_app_mode(self) -> None:
        """First-run auto switch: opening an app shows it full-window (once)."""
        auto, self._auto_app_mode = self._auto_app_mode, False
        if auto and not self._user_switched:
            self.set_app_mode(True)

    def toggle_app_mode(self) -> None:
        self._user_switched = True
        self.set_app_mode(not self._app_mode)

    def toggle_fullscreen(self) -> None:
        """Real fullscreen: the window covers the whole screen (no title bar).

        `state("zoomed")` only maximises the frame — on HiDPI screens that leaves
        a border and a taskbar around the window, so long pages still get cut off
        in screenshots.  The `-fullscreen` attribute is what actually fills the
        display.  Esc, F11 or the 还原 button brings it back.
        """
        self.set_fullscreen(not self._fullscreen)

    def set_fullscreen(self, enabled: bool) -> None:
        if enabled == self._fullscreen:
            if enabled:
                # already on but the WM may have dropped it — force it again
                try:
                    self.root.attributes("-fullscreen", True)
                except tk.TclError:  # pragma: no cover
                    pass
            return
        if enabled:
            try:
                geometry = self.root.winfo_geometry()
                if geometry:
                    self._windowed_geometry = geometry
            except tk.TclError:  # pragma: no cover
                pass
        self._fullscreen = enabled
        try:
            self.root.attributes("-fullscreen", enabled)
            if not enabled and self._windowed_geometry:
                self.root.geometry(self._windowed_geometry)
        except tk.TclError:  # pragma: no cover - platform dependent
            try:
                self.root.state("zoomed" if enabled else "normal")
            except tk.TclError:
                pass
        self.fullscreen_button.configure(text="还原" if enabled else "全屏")
        self._schedule_resize()

    def set_app_mode(self, enabled: bool) -> None:
        """应用模式: the rendered page takes over the window.

        The editor, diagnostics, console and the whole tab strip disappear; only
        the toolbar (address bar / Run / mode button) and status bar remain so the
        user can always switch back with one click or F11.
        """
        if enabled == self._app_mode:
            return
        self._app_mode = enabled
        try:
            if enabled:
                self.notebook.select(self.tab_ui)
                self.paned.forget(self.left_pane)
                self.paned.forget(self.right_pane)
                # re-parent the rendered page behind the toolbar, full width
                self.ui_host.pack_forget()
                self.ui_host.pack(in_=self.root, fill="both", expand=True)
                self.paned.pack_forget()
                # a leaner toolbar in 应用模式: the mode button is the way back
                self.open_button.pack_forget()
                self.run_button.pack_forget()
                self.root.configure(bg=PANEL)
                self.mode_button.configure(text="开发模式")
                # 应用模式 keeps the address bar (it identifies the running app)
                # but drops the editor buttons and the runtime counters
                self.open_button.pack_forget()
                self.run_button.pack_forget()
                self.statusbar.pack_forget()
                self.status_label.pack_forget()
                self.topbar.configure(height=44)
                self.hint_var.set(APP_HINTS)
                self._set_status("")
            else:
                self.ui_host.pack_forget()
                self.ui_host.pack(in_=self.tab_ui, fill="both", expand=True)
                # restore the toolbar in its original order.  `pack -before` needs
                # the reference widget to be managed already, so re-pack the
                # buttons first and then insert the address bar before them.
                self.run_button.pack(
                    side="left", padx=(0, 6), ipady=2, before=self.mode_button
                )
                self.open_button.pack(
                    side="left", padx=(0, 6), ipady=2, before=self.run_button
                )
                self.address_entry.pack(
                    side="left", fill="x", expand=True, padx=(12, 8), ipady=6,
                    before=self.open_button,
                )
                self.status_label.pack(side="right", padx=(0, 14))
                self.statusbar.pack(side="bottom", fill="x")
                self.topbar.configure(height=52)
                self.paned.pack(side="top", fill="both", expand=True, padx=10, pady=(8, 6))
                self.paned.add(self.left_pane, weight=5)
                self.paned.add(self.right_pane, weight=6)
                self.root.configure(bg=BG)
                self.mode_button.configure(text="应用模式")
                self.hint_var.set(DEV_HINTS)
                self._set_status("编辑器已显示")
        except tk.TclError:  # pragma: no cover - defensive
            pass
        self._sync_window_title()
        self._schedule_resize()

    def _statusbar(self) -> None:
        bar = tk.Frame(self.root, bg=PANEL, height=26)
        bar.pack(side="bottom", fill="x")
        bar.pack_propagate(False)
        self.statusbar = bar
        self.hint_var = tk.StringVar(value=DEV_HINTS)
        self.hint_label = tk.Label(
            bar,
            textvariable=self.hint_var,
            fg=MUTED,
            bg=PANEL,
            font=("Segoe UI", 8),
        )
        self.hint_label.pack(side="left", padx=12)

    def _bind_keys(self) -> None:
        self.root.bind("<Control-Return>", lambda _event: self._run_editor())
        self.root.bind("<Control-l>", self._focus_address)
        self.root.bind("<Control-o>", lambda _event: self._open_file())
        self.root.bind("<F11>", lambda _event: self._toggle_app_mode_from_key())
        self.root.bind("<Control-Shift-A>", lambda _event: self._toggle_app_mode_from_key())
        self.root.bind("<Escape>", lambda _event: self._leave_app_mode_from_key())

    def _toggle_app_mode_from_key(self):
        self._user_switched = True
        self.set_app_mode(not self._app_mode)
        return "break"

    def _leave_app_mode_from_key(self):
        # Esc first leaves fullscreen, then app mode — the least surprising order
        if self._fullscreen:
            self.set_fullscreen(False)
            return "break"
        if self._app_mode:
            self._user_switched = True
            self.set_app_mode(False)
            return "break"
        return None

    def _bind_wheel(self, enable: bool) -> None:
        """Bind/unbind the mouse wheel to the ui canvas while the pointer is over it."""
        sequence = "<MouseWheel>"
        if enable:
            self.ui_canvas.bind_all(sequence, self._on_wheel)
        else:
            self.ui_canvas.unbind_all(sequence)

    def _on_wheel(self, event):
        try:
            self.ui_canvas.yview_scroll(int(-event.delta / 120), "units")
        except tk.TclError:  # pragma: no cover
            pass
        return "break"

    def _wrap_width(self, offset: int = 120) -> int:
        """Pixel wrap length that follows the rendered page width."""
        try:
            width = self.ui_canvas.winfo_width()
        except (tk.TclError, AttributeError):  # pragma: no cover
            width = 0
        if width < 320:
            width = 900
        return max(240, width - offset)

    def _resize_app_canvas(self) -> None:
        """Keep the rendered page width in sync with the canvas in both modes."""
        self._resize_job = None
        try:
            width = max(320, self.ui_canvas.winfo_width())
            self.ui_canvas.itemconfigure(self._ui_window, width=width)
        except tk.TclError:  # pragma: no cover
            pass

    def _schedule_resize(self) -> None:
        if self._resize_job is not None:
            try:
                self.root.after_cancel(self._resize_job)
            except tk.TclError:  # pragma: no cover
                pass
        try:
            self._resize_job = self.root.after(30, self._resize_app_canvas)
        except tk.TclError:  # pragma: no cover - window already gone
            self._resize_job = None

    def _focus_address(self, _event=None):
        self.address_entry.focus_set()
        self.address_entry.selection_range(0, "end")
        return "break"

    # ---------------------------------------------------------------- actions
    def _open_url(self, url: str | None = None) -> None:
        target = url or self.address_var.get()
        self.address_var.set(target)
        self._set_status(f"fetching {target} …")
        ok = self.controller.open_url(target)
        if ok:
            self._set_editor(self.controller.last_source)
            self._set_status(f"loaded {target}")
        self._after_execution(ok)

    def _set_editor(self, source: str) -> None:
        self.editor.delete("1.0", "end")
        self.editor.insert("1.0", source)

    def _run_editor(self) -> None:
        source = self.editor.get("1.0", "end-1c")
        self.controller.run_source(source, "<editor>")
        self._after_execution(True)

    def _open_file(self) -> None:
        path = filedialog.askopenfilename(
            title="Open an MI program",
            initialdir=str(ROOT),
            filetypes=[("MI programs", "*.mi"), ("All files", "*.*")],
        )
        if not path:
            return
        try:
            source = Path(path).read_text(encoding="utf-8-sig")
        except OSError as exc:
            self._flash_error(f"cannot read {path}: {exc}")
            return
        self.load_source(source, path)

    def load_source(self, source: str, filename: str = "<editor>") -> None:
        self._set_editor(source)
        self.controller.run_source(source, filename)
        self._after_execution(True)

    def load_example(self, relative: str) -> None:
        path = ROOT / relative
        try:
            source = path.read_text(encoding="utf-8-sig")
        except OSError as exc:
            self._flash_error(f"cannot read {path}: {exc}")
            return
        self.address_var.set(f"local:{relative}")
        self.load_source(source, str(path))

    def _press(self, index: int) -> None:
        self.controller.press(index)
        self.refresh()
        self._schedule_next_timer()

    def _click_row(self, index: int, row: int) -> None:
        self.controller.click_row(index, row)
        self.refresh()
        self._schedule_next_timer()

    def _click_cell(self, index: int, row: int, col: int) -> None:
        self.controller.click_cell(index, row, col)
        self.refresh()
        self._schedule_next_timer()

    def _confirm(self, index: int, yes: bool) -> None:
        self.controller.confirm(index, yes)
        self.refresh()
        self._schedule_next_timer()

    # -------------------------------------------------------------- timers
    def _schedule_next_timer(self) -> None:
        """Arm a Tk timer for the earliest pending view.* timer."""
        if self._timer_job is not None:
            try:
                self.root.after_cancel(self._timer_job)
            except tk.TclError:  # pragma: no cover
                pass
            self._timer_job = None
        pending = self.controller.timers
        if not pending:
            return
        soonest = min(pending, key=lambda timer: timer.due)
        delay = max(50, soonest.due - self.controller.vclock)
        self._timer_job = self.root.after(
            delay, lambda: self._on_timer(max(0, delay))
        )

    def _on_timer(self, elapsed: int) -> None:
        self._timer_job = None
        fired = self.controller.pump(max(1, elapsed))
        if fired:
            self.refresh()
        self._schedule_next_timer()

    def _run_selftest(self) -> None:
        result = run_selftest()
        self.selftest_summary.set(result.summary)
        self.selftest_text.configure(state="normal")
        self.selftest_text.delete("1.0", "end")
        if result.failures:
            self.selftest_text.insert("end", "\n\n".join(result.failures))
        else:
            self.selftest_text.insert(
                "end",
                f"{result.total} 条共享一致性用例全部通过。\n"
                "同一批用例在网页版（网页浏览器/index.html）里也会跑。",
            )
        self.selftest_text.configure(state="disabled")
        self._set_status(result.summary)

    # ----------------------------------------------------------------- render
    def _after_execution(self, ok: bool) -> None:
        self.refresh()
        self._schedule_next_timer()
        if ok:
            self.notebook.select(self.tab_ui)

    def refresh(self) -> None:
        if self._refreshing:
            return
        self._refreshing = True
        try:
            self._render_console()
            self._render_diagnostics()
            self._render_ui()
            self._render_status()
            self._sync_window_title()
        finally:
            self._refreshing = False

    def _sync_window_title(self) -> None:
        """Let the running app name the window (falls back to the browser name)."""
        title = (self.controller.view_title or "").strip()
        if title and self._app_mode:
            self.root.title(title)
        elif not self._app_mode:
            self.root.title(DEFAULT_TITLE)

    def _render_console(self) -> None:
        self.console.configure(state="normal")
        self.console.delete("1.0", "end")
        for line in self.controller.output:
            self.console.insert("end", line + "\n")
        self.console.configure(state="disabled")
        self.console.see("end")

    def _render_diagnostics(self) -> None:
        self.diagnostics.configure(state="normal")
        self.diagnostics.delete("1.0", "end")
        source = self.controller.last_source
        filename = self.controller.last_filename
        for diag in self.controller.diagnostics:
            tag = "warn" if diag.severity == "warning" else "error"
            self.diagnostics.tag_configure("error", foreground=ERROR)
            self.diagnostics.tag_configure("warn", foreground=WARN)
            self.diagnostics.insert("end", diag.render(source, filename) + "\n\n", tag)
        if self.controller.error is not None:
            self.diagnostics.tag_configure("error", foreground=ERROR)
            self.diagnostics.insert(
                "end",
                render_runtime_error(self.controller.error, source, filename) + "\n",
                "error",
            )
        self.diagnostics.configure(state="disabled")

    def _render_ui(self) -> None:
        for child in self.ui_inner.winfo_children():
            child.destroy()
        self._input_vars = {}
        self._containers = {}
        if not self.controller.ui_elements:
            tk.Label(
                self.ui_inner,
                text="尚未渲染。运行一个使用 ui.per / view.screen 的程序。",
                fg=MUTED,
                bg=PANEL,
                font=MONO_SMALL,
                anchor="w",
                justify="left",
            ).pack(fill="x", padx=18, pady=14)
            return
        for element in self.controller.ui_elements:
            self._render_element(element)

    def _render_element(self, element) -> None:
        parent = self._parent_for(element)
        if element.kind == "screen":
            self._render_screen(element)
        elif element.kind == "card":
            self._render_card(element, parent)
        elif element.kind == "row":
            self._render_row(element, parent)
        elif element.kind == "heading":
            tk.Label(
                parent,
                text=element.value,
                fg=BLUE,
                bg=PANEL,
                font=("Segoe UI", 15, "bold"),
                anchor="w",
                justify="left",
                wraplength=self._wrap_width(),
            ).pack(fill="x", padx=18, pady=(14, 4))
        elif element.kind == "text":
            tk.Label(
                parent,
                text=element.value,
                fg=TEXT,
                bg=PANEL,
                font=("Segoe UI", 10),
                anchor="w",
                justify="left",
                wraplength=self._wrap_width(),
            ).pack(fill="x", padx=18, pady=3)
        elif element.kind == "input":
            self._render_input(element, parent)
        elif element.kind == "button":
            self._render_button(element, parent)
        elif element.kind == "mount":
            tk.Label(
                parent,
                text="● mounted" + (f" → {element.value}" if element.value else ""),
                fg=ACCENT,
                bg=PANEL,
                font=MONO_SMALL,
                anchor="w",
            ).pack(fill="x", padx=18, pady=(10, 0))
        elif element.kind == "notice":
            self._render_notice(element)
        elif element.kind == "list":
            self._render_list(element, parent)
        elif element.kind == "table":
            self._render_table(element, parent)
        elif element.kind == "progress":
            self._render_progress(element, parent)
        elif element.kind == "badge":
            self._render_badge(element, parent)
        elif element.kind == "map":
            self._render_map(element, parent)
        elif element.kind == "image":
            self._render_image(element, parent)
        elif element.kind == "timer":
            self._render_timer(element, parent)
        elif element.kind == "confirm":
            self._render_confirm(element, parent)

    # -- containers ---------------------------------------------------------
    def _parent_for(self, element):
        parent_index = getattr(element, "parent", -1)
        if parent_index is not None and parent_index >= 0:
            frame = self._containers.get(parent_index)
            if frame is not None:
                return frame
        return self.ui_inner

    def _render_screen(self, element) -> None:
        head = tk.Frame(self.ui_inner, bg=PANEL)
        head.pack(fill="x", padx=16, pady=(14, 2))
        tk.Label(
            head,
            text=element.label,
            fg=TEXT,
            bg=PANEL,
            font=("Microsoft YaHei UI", 18, "bold"),
            anchor="w",
        ).pack(side="left")
        if element.value:
            tk.Label(
                head,
                text="  " + element.value,
                fg=MUTED,
                bg=PANEL,
                font=("Microsoft YaHei UI", 10),
                anchor="w",
            ).pack(side="left", pady=(6, 0))
        tk.Frame(self.ui_inner, bg=BORDER, height=1).pack(fill="x", padx=16, pady=(6, 4))

    def _render_card(self, element, parent) -> None:
        outer = tk.Frame(parent, bg=PANEL2)
        outer.pack(fill="x", padx=16, pady=6)
        head = tk.Frame(outer, bg=PANEL2)
        head.pack(fill="x", padx=12, pady=(8, 2))
        tk.Label(
            head,
            text=element.label,
            fg=BLUE,
            bg=PANEL2,
            font=("Microsoft YaHei UI", 12, "bold"),
            anchor="w",
        ).pack(side="left")
        if element.value:
            tk.Label(
                head,
                text="  " + element.value,
                fg=MUTED,
                bg=PANEL2,
                font=("Microsoft YaHei UI", 9),
                anchor="w",
            ).pack(side="left")
        body = tk.Frame(outer, bg=PANEL2)
        body.pack(fill="x", padx=4, pady=(0, 8))
        self._containers[element.index] = body

    def _render_row(self, element, parent) -> None:
        frame = tk.Frame(parent, bg=PANEL2)
        frame.pack(fill="x", padx=12, pady=4)
        self._containers[element.index] = frame

    def _render_notice(self, element) -> None:
        colours = {
            "ok": ("#0b2e16", ACCENT),
            "warn": ("#2f2410", WARN),
            "bad": ("#2f1113", ERROR),
            "info": ("#132535", BLUE),
        }
        bg, fg = colours.get(element.tone, colours["info"])
        bar = tk.Frame(self.ui_inner, bg=bg)
        bar.pack(fill="x", padx=16, pady=(6, 2))
        tk.Label(
            bar,
            text=element.value,
            fg=fg,
            bg=bg,
            font=("Microsoft YaHei UI", 10),
            anchor="w",
            justify="left",
            wraplength=self._wrap_width(),
            padx=10,
            pady=6,
        ).pack(fill="x")

    # -- widgets ------------------------------------------------------------
    def _render_input(self, element, parent) -> None:
        row = tk.Frame(parent, bg=PANEL2)
        row.pack(fill="x", padx=14, pady=4)
        tk.Label(
            row,
            text=element.label,
            fg=MUTED,
            bg=PANEL2,
            font=("Microsoft YaHei UI", 10),
            width=10,
            anchor="w",
        ).pack(side="left")
        var = tk.StringVar(value=element.value)
        entry = tk.Entry(
            row,
            textvariable=var,
            bg=INPUT_BG,
            fg=TEXT,
            insertbackground=TEXT,
            relief="flat",
            bd=0,
            highlightthickness=1,
            highlightbackground=BORDER,
            highlightcolor=BLUE,
            font=MONO_SMALL,
        )
        secret = bool(element.data.get("secret")) if isinstance(element.data, dict) else False
        if secret:
            entry.configure(show="●")
        entry.pack(side="left", fill="x", expand=True, ipady=4)
        label = element.label

        def _sync(_name, _index, _mode, var=var, label=label):
            self.controller.set_input(label, var.get())

        var.trace_add("write", _sync)
        self._input_vars[label] = var

    def _render_button(self, element, parent) -> None:
        tk.Button(
            parent,
            text=element.label,
            command=lambda index=element.index: self._press(index),
            bg=ACCENT,
            fg=ACCENT_DARK,
            activebackground="#56d364",
            activeforeground=ACCENT_DARK,
            relief="flat",
            bd=0,
            padx=14,
            pady=5,
            cursor="hand2",
            font=("Microsoft YaHei UI", 10),
        ).pack(side="left" if element.parent >= 0 and self._is_row(element.parent) else "top",
               anchor="w", padx=6, pady=5)

    def _is_row(self, index: int) -> bool:
        if not (0 <= index < len(self.controller.ui_elements)):
            return False
        return self.controller.ui_elements[index].kind == "row"

    def _render_list(self, element, parent) -> None:
        box = tk.Frame(parent, bg=PANEL2)
        box.pack(fill="x", padx=14, pady=4)
        data = element.data if isinstance(element.data, dict) else {}
        for number, item in enumerate(data.get("items", []), start=1):
            row = tk.Frame(box, bg=PANEL2)
            row.pack(fill="x", pady=1)
            if data.get("numbered"):
                tk.Label(
                    row, text=f"{number}.", fg=MUTED, bg=PANEL2, font=MONO_SMALL, width=3
                ).pack(side="left", anchor="n")
            tk.Label(
                row,
                text=item["text"],
                fg=self._tone_colour(item.get("tone")),
                bg=PANEL2,
                font=("Microsoft YaHei UI", 10),
                anchor="w",
                justify="left",
                wraplength=self._wrap_width(offset=40),
            ).pack(side="left", fill="x", expand=True)
            if item.get("detail"):
                tk.Label(
                    row,
                    text=item["detail"],
                    fg=MUTED,
                    bg=PANEL2,
                    font=("Microsoft YaHei UI", 9),
                    anchor="w",
                    justify="left",
                    wraplength=self._wrap_width(offset=40),
                ).pack(side="left", padx=(8, 0))

    @staticmethod
    def _tone_colour(tone: str) -> str:
        return {"ok": ACCENT, "warn": WARN, "bad": ERROR, "info": TEXT}.get(tone, TEXT)

    def _render_table(self, element, parent) -> None:
        data = element.data if isinstance(element.data, dict) else {}
        wrapper = tk.Frame(parent, bg=PANEL2)
        wrapper.pack(fill="x", padx=14, pady=6)
        columns = data.get("columns", [])
        rows = data.get("rows", [])
        for col, name in enumerate(columns):
            tk.Label(
                wrapper,
                text=str(name),
                fg=BLUE,
                bg=PANEL2,
                font=("Microsoft YaHei UI", 10, "bold"),
                anchor="w",
                width=14 if len(columns) > 3 else 20,
            ).grid(row=0, column=col, sticky="w", padx=4, pady=(2, 4))
        for r, row in enumerate(rows, start=1):
            for c, cell in enumerate(row[: len(columns)]):
                tk.Label(
                    wrapper,
                    text=str(cell),
                    fg=TEXT,
                    bg=PANEL2,
                    font=("Microsoft YaHei UI", 9),
                    anchor="w",
                    width=14 if len(columns) > 3 else 20,
                ).grid(row=r, column=c, sticky="w", padx=4, pady=1)
            if element.station is not None:
                tk.Button(
                    wrapper,
                    text="选择",
                    command=lambda index=element.index, r=r: self._click_row(index, r),
                    bg=PANEL2,
                    fg=BLUE,
                    activebackground=BORDER,
                    relief="flat",
                    bd=0,
                    font=("Microsoft YaHei UI", 9),
                    cursor="hand2",
                ).grid(row=r, column=len(columns), padx=4, sticky="w")
        if not rows:
            tk.Label(
                wrapper, text="（暂无数据）", fg=MUTED, bg=PANEL2, font=MONO_SMALL, anchor="w"
            ).grid(row=1, column=0, sticky="w", padx=4)

    def _render_progress(self, element, parent) -> None:
        row = tk.Frame(parent, bg=PANEL2)
        row.pack(fill="x", padx=14, pady=4)
        tk.Label(
            row, text=element.label, fg=TEXT, bg=PANEL2, font=("Microsoft YaHei UI", 10),
            anchor="w", width=16,
        ).pack(side="left")
        value = element.data.get("value", 0.0) if isinstance(element.data, dict) else 0.0
        canvas = tk.Canvas(
            row, height=14, bg=INPUT_BG, bd=0, highlightthickness=1,
            highlightbackground=BORDER,
        )
        canvas.pack(side="left", fill="x", expand=True, padx=6)

        def _draw(_event=None, canvas=canvas, value=value, tone=element.tone):
            width = max(1, canvas.winfo_width() - 2)
            canvas.delete("all")
            canvas.create_rectangle(
                1, 1, max(2, int(width * value)), 13,
                fill=self._tone_colour(tone), outline="",
            )

        canvas.bind("<Configure>", _draw)
        tk.Label(
            row, text=f"{value * 100:.0f}%", fg=self._tone_colour(element.tone),
            bg=PANEL2, font=MONO_SMALL, width=5,
        ).pack(side="left")

    def _render_badge(self, element, parent) -> None:
        colours = {
            "ok": ("#0b2e16", ACCENT),
            "warn": ("#2f2410", WARN),
            "bad": ("#2f1113", ERROR),
            "info": ("#132535", BLUE),
        }
        bg, fg = colours.get(element.tone, colours["info"])
        side = "left" if element.parent >= 0 and self._is_row(element.parent) else "top"
        tk.Label(
            parent,
            text=f" {element.value} ",
            fg=fg,
            bg=bg,
            font=("Microsoft YaHei UI", 9),
            padx=4,
            pady=2,
        ).pack(side=side, anchor="w", padx=6, pady=4)

    def _render_map(self, element, parent) -> None:
        data = element.data if isinstance(element.data, dict) else {}
        cells = data.get("cells", [])
        legend = data.get("legend", {})
        box = tk.Frame(parent, bg=PANEL2)
        box.pack(fill="x", padx=14, pady=6)
        palette = {
            "W": ("#30363d", MUTED),
            "D": ("#1f4e79", BLUE),
            "E": ("#0b2e16", ACCENT),
            "F": ("#5a1d1d", "#ff7b72"),
            "S": ("#3a3a3a", "#d2a8ff"),
            "P": ("#133f2a", "#7ee787"),
            "X": ("#4a2d0b", WARN),
            ".": (INPUT_BG, MUTED),
        }
        for r, row in enumerate(cells):
            line = tk.Frame(box, bg=PANEL2)
            line.pack(anchor="w")
            for c, code in enumerate(row):
                bg, fg = palette.get(code, (INPUT_BG, MUTED))
                tk.Button(
                    line,
                    text=code if code != "." else " ",
                    width=3,
                    height=1,
                    bg=bg,
                    fg=fg,
                    activebackground=BORDER,
                    relief="flat",
                    bd=0,
                    font=MONO_SMALL,
                    command=lambda index=element.index, r=r + 1, c=c + 1: self._click_cell(
                        index, r, c
                    ),
                ).pack(side="left", padx=1, pady=1)
        legend_row = tk.Frame(box, bg=PANEL2)
        legend_row.pack(anchor="w", pady=(6, 0))
        for code, meaning in legend.items():
            bg, fg = palette.get(code, (INPUT_BG, MUTED))
            tk.Label(
                legend_row, text=f" {code} ", fg=fg, bg=bg, font=MONO_SMALL, padx=2
            ).pack(side="left", padx=(0, 2))
            tk.Label(
                legend_row, text=str(meaning), fg=MUTED, bg=PANEL2,
                font=("Microsoft YaHei UI", 9),
            ).pack(side="left", padx=(0, 10))

    def _render_image(self, element, parent) -> None:
        data = element.data if isinstance(element.data, dict) else {}
        width = data.get("width")
        path = element.value
        try:
            from PIL import Image, ImageTk

            image = Image.open(path)
            if width:
                ratio = width / max(1, image.width)
                image = image.resize((int(width), max(1, int(image.height * ratio))))
            photo = ImageTk.PhotoImage(image)
            label = tk.Label(parent, image=photo, bg=PANEL2, bd=0)
            label.image = photo  # keep a reference
            label.pack(anchor="w", padx=14, pady=6)
        except Exception:  # noqa: BLE001 - placeholder on any failure
            tk.Label(
                parent,
                text=f"［图片不可用］{path}",
                fg=MUTED,
                bg=PANEL2,
                font=("Microsoft YaHei UI", 9),
                anchor="w",
            ).pack(fill="x", padx=14, pady=4)

    def _render_timer(self, element, parent) -> None:
        left = self.controller.remaining(element.index)
        row = tk.Frame(parent, bg=PANEL2)
        row.pack(fill="x", padx=14, pady=4)
        tk.Label(
            row, text=element.label, fg=TEXT, bg=PANEL2,
            font=("Microsoft YaHei UI", 10), anchor="w",
        ).pack(side="left")
        colour = ERROR if left <= 60 else WARN if left <= 300 else ACCENT
        tk.Label(
            row,
            text=f"{left // 60:02d}:{left % 60:02d}",
            fg=colour,
            bg=PANEL2,
            font=("Consolas", 12, "bold"),
        ).pack(side="left", padx=10)

    def _render_confirm(self, element, parent) -> None:
        box = tk.Frame(parent, bg=PANEL2)
        box.pack(fill="x", padx=14, pady=6)
        tk.Label(
            box,
            text=element.value,
            fg=TEXT,
            bg=PANEL2,
            font=("Microsoft YaHei UI", 10),
            anchor="w",
            justify="left",
            wraplength=self._wrap_width(offset=100),
        ).pack(fill="x")
        buttons = tk.Frame(box, bg=PANEL2)
        buttons.pack(anchor="w", pady=(4, 0))
        for text, answer, colour in (("确定", True, ACCENT), ("取消", False, PANEL2)):
            tk.Button(
                buttons,
                text=text,
                command=lambda index=element.index, answer=answer: self._confirm(index, answer),
                bg=colour,
                fg=ACCENT_DARK if answer else TEXT,
                activebackground=BORDER,
                relief="flat",
                bd=0,
                padx=14,
                pady=4,
                cursor="hand2",
                font=("Microsoft YaHei UI", 9),
            ).pack(side="left", padx=(0, 8))

    def _render_status(self) -> None:
        controller = self.controller
        if controller.error is not None:
            self._set_status(f"runtime error [{controller.error.code}]", colour=ERROR)
            return
        errors = [d for d in controller.diagnostics if d.severity == "error"]
        warnings = [d for d in controller.diagnostics if d.severity == "warning"]
        if errors:
            self._set_status(f"{len(errors)} error(s), {len(warnings)} warning(s)", colour=ERROR)
        elif warnings:
            self._set_status(f"ok · {len(warnings)} warning(s)", colour=WARN)
        else:
            count = len(controller.ui_elements)
            self._set_status(f"ready · {count} ui element(s)", colour=MUTED)

    def _set_status(self, text: str, colour: str = MUTED) -> None:
        self.status_var.set(text)
        self.status_label.configure(fg=colour)

    def _flash_error(self, message: str) -> None:
        self._set_status(message, colour=ERROR)

    def _render_reference(self) -> None:
        self.reference.configure(state="normal")
        self.reference.delete("1.0", "end")
        current = None
        for group, canonical, spellings, summary in TABLE.all_spelling_rows():
            if group != current:
                current = group
                self.reference.insert("end", f"\n[{group}s]\n")
            self.reference.insert("end", f"{canonical:<14} {spellings}\n")
        self.reference.configure(state="disabled")



class _中文帮助(argparse.RawDescriptionHelpFormatter):
    """argparse 的固定文案中文化（用法、选项、帮助本身），并保留 epilog 的原始换行。"""

    def _format_usage(self, usage, actions, groups, prefix):  # noqa: D401
        return super()._format_usage(usage, actions, groups, prefix or "用法：")


class _中文参数(argparse.ArgumentParser):
    def __init__(self, *args, **kwargs):
        kwargs.setdefault("formatter_class", _中文帮助)
        kwargs.setdefault("add_help", False)
        super().__init__(*args, **kwargs)
        self.add_argument("-h", "--help", action="help", help="显示本帮助并退出")

    def format_help(self) -> str:
        text = super().format_help()
        return text.replace("options:", "选项：").replace("positional arguments:", "位置参数：")


def _program() -> str:
    """冻结成 exe 时叫 mi-browser，从源码跑时是 python -m 米.浏览器。"""
    if getattr(sys, "frozen", False):
        return "mi-browser"
    return "python -m 米.浏览器"


def main(argv: list[str] | None = None) -> int:
    from ..cli import _utf8

    _utf8()  # 让 --help / --selftest 的中文在 Windows 控制台里不乱码
    parser = _中文参数(
        prog=_program(),
        description="Mi Browser — MI 的专用浏览器（桌面版）。",
        epilog=(
            "示例：\n"
            "  mi-browser                              打开开发模式（编辑器 + 标签页）\n"
            "  mi-browser 程序.mi                       打开一个本地 MI 程序（也能直接拖进来）\n"
            "  mi-browser --open http://127.0.0.1:8080/app/forum.mi\n"
            "  mi-browser --selftest                    无窗口跑 63 条一致性自测\n"
        ),
    )
    parser.add_argument("--open", metavar="URL", help="通过 HTTP 打开一个 MI 应用")
    parser.add_argument("--source", metavar="FILE", help="打开一个本地 .mi 程序（也可直接把文件拖到图标上）")
    parser.add_argument("--store", metavar="FILE", help="store.* 使用的持久化文件")
    parser.add_argument("--selftest", action="store_true", help="无窗口运行一致性自测")
    parser.add_argument("--size", default="1440x900", help="窗口尺寸，例如 1440x900")
    parser.add_argument(
        "--mode",
        choices=("auto", "app", "dev"),
        default="auto",
        help="auto（默认）：给了 --source/--open 就进应用模式；app：强制应用模式；dev：强制开发模式",
    )
    parser.add_argument("--fullscreen", action="store_true", help="启动即真实全屏")

    # 把 .mi 文件拖到 exe 图标上时，argv 只有一个裸路径 —— 等价于 --source 路径
    raw = list(sys.argv[1:] if argv is None else argv)
    if len(raw) == 1 and not raw[0].startswith("-") and raw[0].lower().endswith(".mi"):
        raw = ["--source", raw[0]]
    args = parser.parse_args(raw)

    if args.store:
        os.environ["MI_STORE_FILE"] = args.store

    if args.selftest:
        result = run_selftest()
        print(result.summary)
        for failure in result.failures:
            print(" -", failure)
        return 0 if not result.failures else 1

    try:
        root = tk.Tk()
    except tk.TclError as exc:  # pragma: no cover - headless environments
        print(f"cannot open a window: {exc}", file=sys.stderr)
        return 3
    app = MiBrowserApp(root, open_url=args.open, source_path=args.source, size=args.size)
    if args.fullscreen:
        app.set_fullscreen(True)
    if args.mode == "app":
        app.set_app_mode(True)
    elif args.mode == "dev":
        app._user_switched = True
        app.set_app_mode(False)
    try:
        root.mainloop()
    finally:
        try:
            root.destroy()
        except tk.TclError:  # pragma: no cover
            pass
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
