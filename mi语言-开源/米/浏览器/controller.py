"""Mi Browser — the Python edition.

``BrowserController`` is the headless core: it runs MI programs on the
reference Python runtime, implements the browser ``ui.*`` and desktop
``view.*`` hosts as a rendered element model, and opens apps over HTTP
(``GET /app/xxx.mi`` from an MI backend).  ``tk_app`` is the Tkinter shell;
``python -m 米.浏览器`` opens it.

The JavaScript edition (``网页浏览器/index.html``) and this one share the same MI
semantics and the same ``ui.*`` host contract; the difference is the runtime and
the window toolkit.  ``view.*`` is Python-only: it needs real widgets, timers and
an in-memory view session.
"""
from __future__ import annotations

import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Callable

from ..api import compile_source
from ..errors import Diagnostic, MiRuntimeError
from ..evaluator import ExecutionOptions, Runtime, execute
from ..宿主.python_host import IMPLS as PYTHON_HOST
from ..modules import ModuleLoader
from ..tokens import Position, Span
from ..values import (
    NOTHING,
    Assembly,
    BuiltinValue,
    MiValueError,
    StationValue,
    as_int,
    as_text,
    display,
)

UIElementKind = str

_NO_VALUE = object()

TONES = ("ok", "warn", "bad", "info")


def _dummy_span() -> Span:
    return Span(Position(1, 1, 0), Position(1, 1, 0))


@dataclass
class UIElement:
    """One rendered widget.

    ``kind`` is either a classic ``ui.*`` kind (mount/heading/text/input/button)
    or a ``view.*`` kind (screen/card/row/list/table/progress/badge/map/image/
    timer/notice/confirm).  ``parent`` is the index of the enclosing row or card
    (-1 for top level) and ``data`` holds kind-specific payloads.
    """

    kind: UIElementKind
    label: str = ""
    value: str = ""
    station: Any = None
    payload: Any = _NO_VALUE
    index: int = 0
    button_id: int = 0
    tone: str = ""
    parent: int = -1
    data: Any = None


@dataclass
class Timer:
    handle: int
    station: Any
    value: Any
    interval: int
    repeat: bool
    due: int
    element: int = -1
    label: str = ""
    seconds: int = 0


@dataclass
class BrowserState:
    diagnostic_lines: list[str] = field(default_factory=list)


class BrowserController:
    """Headless Mi Browser: runs MI, owns the element model and view session."""

    def __init__(
        self,
        *,
        on_output: Callable[[str], None] | None = None,
        on_change: Callable[[], None] | None = None,
        max_steps: int = 5_000_000,
        max_depth: int = 400,
    ) -> None:
        self.on_output = on_output
        self.on_change = on_change
        self.max_steps = max_steps
        self.max_depth = max_depth
        self.loader = ModuleLoader()
        self.ui_elements: list[UIElement] = []
        self.inputs: dict[str, str] = {}
        self.view_state: dict[str, Any] = {}
        self.timers: list[Timer] = []
        self._vclock = 0
        self._timer_seq = 0
        self._card = -1
        self._row = -1
        self._button_counter = 0
        self.view_title = ""
        self.view_subtitle = ""
        self.output: list[str] = []
        self.diagnostics: list[Diagnostic] = []
        self.error: MiRuntimeError | None = None
        self.last_source = ""
        self.last_filename = "<browser>"
        self.address = ""
        self.hosts: dict[str, Any] = dict(PYTHON_HOST)
        self.hosts.update(self._ui_hosts())
        self.hosts.update(self._view_hosts())
        self.runtime = self._new_runtime()

    # ------------------------------------------------------------------ setup
    def _new_runtime(self) -> Runtime:
        return Runtime(
            loader=self.loader,
            hosts=self.hosts,
            # the desktop browser is both hosts at once: it renders ui.*/view.*
            # like a browser, and it has the Python host's store/fs/env/net ports.
            host_name="python+browser",
            options=ExecutionOptions(max_steps=self.max_steps, max_depth=self.max_depth),
            on_output=self._emit,
        )

    def _emit(self, line: str) -> None:
        self.output.append(line)
        if self.on_output is not None:
            self.on_output(line)

    def _changed(self) -> None:
        if self.on_change is not None:
            self.on_change()

    # ------------------------------------------------------------- ui host
    def _ui_hosts(self) -> dict[str, Callable[..., Any]]:
        return {
            "ui.mount": self._ui_mount,
            "ui.clear": self._ui_clear,
            "ui.heading": self._ui_heading,
            "ui.text": self._ui_text,
            "ui.input": self._ui_input,
            "ui.value": self._ui_value,
            "ui.button": self._ui_button,
        }

    def _add(self, element: UIElement) -> UIElement:
        element.index = len(self.ui_elements)
        element.parent = self._parent()
        self.ui_elements.append(element)
        return element

    def _parent(self) -> int:
        if self._row >= 0:
            return self._row
        return self._card

    def _reset_view(self) -> None:
        self.ui_elements.clear()
        self.inputs.clear()
        self.timers.clear()
        self._button_counter = 0
        self._timer_seq = 0
        self._card = -1
        self._row = -1
        self.view_title = ""
        self.view_subtitle = ""

    def _ui_mount(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        target = bound.get("target")
        self._add(
            UIElement("mount", value="" if target is None else display(target))
        )
        return NOTHING

    def _ui_clear(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        self._reset_view()
        return NOTHING

    def _ui_heading(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        self._add(UIElement("heading", value=display(bound.get("value"))))
        return NOTHING

    def _ui_text(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        self._add(UIElement("text", value=display(bound.get("value"))))
        return NOTHING

    def _ui_input(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        label = as_text(bound.get("label"), "`ui.input`'s `label`")
        initial = bound.get("value")
        text = "" if initial is None else display(initial)
        self.inputs[label] = text
        self._add(UIElement("input", label=label, value=text, data={"secret": False}))
        return NOTHING

    def _ui_value(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        label = as_text(bound.get("label"), "`ui.value`'s `label`")
        if label not in self.inputs:
            raise MiValueError(f"there is no input labelled `{label}`")
        return self.inputs[label]

    def _ui_button(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        label = as_text(bound.get("label"), "`ui.button`'s `label`")
        station = bound.get("press")
        if not isinstance(station, (StationValue, BuiltinValue)):
            raise MiValueError(
                "`ui.button`'s `press` must be a station (or a builtin)"
            )
        payload = bound["value"] if "value" in bound else _NO_VALUE
        element = self._add(
            UIElement("button", label=label, station=station, payload=payload)
        )
        self._button_counter += 1
        element.button_id = self._button_counter
        return NOTHING

    # ----------------------------------------------------------- view host
    def _view_hosts(self) -> dict[str, Callable[..., Any]]:
        return {
            "view.screen": self._view_screen,
            "view.card": self._view_card,
            "view.row": self._view_row,
            "view.list": self._view_list,
            "view.table": self._view_table,
            "view.progress": self._view_progress,
            "view.badge": self._view_badge,
            "view.map": self._view_map,
            "view.image": self._view_image,
            "view.timer": self._view_timer,
            "view.after": self._view_after,
            "view.every": self._view_every,
            "view.cancel": self._view_cancel,
            "view.keep": self._view_keep,
            "view.recall": self._view_recall,
            "view.notice": self._view_notice,
            "view.confirm": self._view_confirm,
        }

    @staticmethod
    def _tone(value: Any, default: str = "info") -> str:
        if value is None or value is NOTHING:
            return default
        text = str(display(value)).strip().lower()
        return text if text in TONES else default

    @staticmethod
    def _station_arg(value: Any, what: str) -> Any:
        if not isinstance(value, (StationValue, BuiltinValue)):
            raise MiValueError(f"`{what}` must be a station")
        return value

    def _view_screen(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        title = as_text(bound.get("title"), "`view.screen`'s `title`")
        subtitle = bound.get("subtitle")
        self._reset_view()
        self.view_title = title
        self.view_subtitle = "" if subtitle is None else display(subtitle)
        self._add(
            UIElement("screen", label=title, value=self.view_subtitle)
        )
        return NOTHING

    def _view_card(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        title = as_text(bound.get("title"), "`view.card`'s `title`")
        subtitle = bound.get("subtitle")
        element = UIElement(
            "card",
            label=title,
            value="" if subtitle is None else display(subtitle),
            parent=self._card,
        )
        element.index = len(self.ui_elements)
        self.ui_elements.append(element)
        self._card = element.index
        self._row = -1
        return NOTHING

    def _view_row(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        element = UIElement("row", parent=self._card)
        element.index = len(self.ui_elements)
        self.ui_elements.append(element)
        self._row = element.index
        return NOTHING

    def _view_list(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        raw = bound.get("items")
        if not isinstance(raw, tuple):
            raise MiValueError("`view.list`'s `items` must be a list")
        items = []
        for entry in raw:
            if isinstance(entry, Assembly):
                items.append(
                    {
                        "text": display(entry.get("text")) if entry.has("text") else display(entry),
                        "detail": display(entry.get("detail")) if entry.has("detail") else "",
                        "tone": self._tone(entry.get("tone") if entry.has("tone") else None),
                    }
                )
            else:
                items.append({"text": display(entry), "detail": "", "tone": "info"})
        numbered = bound.get("numbered")
        self._add(
            UIElement(
                "list",
                data={"items": items, "numbered": bool(numbered)},
            )
        )
        return NOTHING

    def _view_table(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        columns = bound.get("columns")
        rows = bound.get("rows")
        if not isinstance(columns, tuple) or not isinstance(rows, tuple):
            raise MiValueError("`view.table` needs `columns` and `rows` lists")
        table_rows = []
        for row in rows:
            if not isinstance(row, (tuple, Assembly)):
                raise MiValueError("each `view.table` row must be a list or assembly")
            table_rows.append(list(row) if isinstance(row, tuple) else [v for _, v in row.fields])
        on_click = bound.get("on_click")
        if on_click is not None and on_click is not NOTHING:
            on_click = self._station_arg(on_click, "view.table`'s `on_click")
        id_field = bound.get("id_field")
        self._add(
            UIElement(
                "table",
                label="",
                station=on_click,
                data={
                    "columns": [display(c) for c in columns],
                    "rows": table_rows,
                    "id_field": None if id_field is None else display(id_field),
                },
            )
        )
        return NOTHING

    def _view_progress(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        label = as_text(bound.get("label"), "`view.progress`'s `label`")
        raw = bound.get("value")
        if not isinstance(raw, (int, float)) or isinstance(raw, bool):
            raise MiValueError("`view.progress`'s `value` must be a number")
        value = float(raw)
        if value > 1.0:
            value = value / 100.0
        value = max(0.0, min(1.0, value))
        tone = bound.get("tone")
        if tone is None or tone is NOTHING:
            tone = "ok" if value >= 0.9 else "warn" if value >= 0.6 else "bad"
        self._add(
            UIElement(
                "progress",
                label=label,
                value=f"{value:.2f}",
                tone=self._tone(tone),
                data={"value": value},
            )
        )
        return NOTHING

    def _view_badge(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        text = as_text(bound.get("text"), "`view.badge`'s `text`")
        self._add(
            UIElement("badge", value=text, tone=self._tone(bound.get("tone")))
        )
        return NOTHING

    def _view_map(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        cells = bound.get("cells")
        if not isinstance(cells, tuple) or not all(isinstance(row, tuple) for row in cells):
            raise MiValueError("`view.map`'s `cells` must be a list of lists")
        grid = [[display(cell) for cell in row] for row in cells]
        legend = bound.get("legend")
        legend_map: dict[str, str] = {}
        if isinstance(legend, Assembly):
            for code, meaning in legend.fields:
                legend_map[code] = display(meaning)
        elif legend is not None and legend is not NOTHING:
            raise MiValueError("`view.map`'s `legend` must be an assembly")
        on_click = bound.get("on_click")
        if on_click is None or on_click is NOTHING:
            raise MiValueError("`view.map` needs an `on_click` station")
        self._add(
            UIElement(
                "map",
                station=self._station_arg(on_click, "view.map`'s `on_click"),
                data={"cells": grid, "legend": legend_map},
            )
        )
        return NOTHING

    def _view_image(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        path = as_text(bound.get("path"), "`view.image`'s `path`")
        width = bound.get("width")
        self._add(
            UIElement(
                "image",
                value=path,
                data={"width": None if width is None else as_int(width, "`view.image`'s `width`")},
            )
        )
        return NOTHING

    def _view_timer(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        label = as_text(bound.get("label"), "`view.timer`'s `label`")
        seconds = as_int(bound.get("seconds"), "`view.timer`'s `seconds`")
        on_done = self._station_arg(bound.get("on_done"), "view.timer`'s `on_done")
        element = self._add(
            UIElement(
                "timer",
                label=label,
                value=str(seconds),
                station=on_done,
                data={"seconds": seconds},
            )
        )
        timer = self._schedule(seconds * 1000, on_done, NOTHING, False)
        timer.element = element.index
        timer.label = label
        timer.seconds = seconds
        return NOTHING

    def _schedule(
        self, ms: int, station: Any, value: Any, repeat: bool
    ) -> Timer:
        if ms < 0:
            raise MiValueError("timer delay cannot be negative")
        self._timer_seq += 1
        timer = Timer(
            handle=self._timer_seq,
            station=self._station_arg(station, "timer"),
            value=value,
            interval=ms,
            repeat=repeat,
            due=self._vclock + ms,
        )
        self.timers.append(timer)
        return timer

    def _view_after(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        ms = as_int(bound.get("ms"), "`view.after`'s `ms`")
        station = self._station_arg(bound.get("action"), "view.after`'s `action`")
        value = bound.get("value", NOTHING)
        return self._schedule(ms, station, value, False).handle

    def _view_every(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        ms = as_int(bound.get("ms"), "`view.every`'s `ms`")
        if ms <= 0:
            raise MiValueError("`view.every`'s `ms` must be positive")
        station = self._station_arg(bound.get("action"), "view.every`'s `action`")
        value = bound.get("value", NOTHING)
        return self._schedule(ms, station, value, True).handle

    def _view_cancel(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        handle = as_int(bound.get("handle"), "`view.cancel`'s `handle`")
        self.timers = [timer for timer in self.timers if timer.handle != handle]
        return NOTHING

    def _view_keep(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        name = as_text(bound.get("name"), "`view.keep`'s `name`")
        self.view_state[name] = bound.get("value", NOTHING)
        return NOTHING

    def _view_recall(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        name = as_text(bound.get("name"), "`view.recall`'s `name`")
        if name in self.view_state:
            return self.view_state[name]
        return bound.get("fallback", NOTHING)

    def _view_notice(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        text = as_text(bound.get("text"), "`view.notice`'s `text`")
        self._add(UIElement("notice", value=text, tone=self._tone(bound.get("tone"))))
        return NOTHING

    def _view_confirm(self, runtime: Any, bound: dict[str, Any], span: Any) -> Any:
        text = as_text(bound.get("text"), "`view.confirm`'s `text`")
        on_yes = self._station_arg(bound.get("on_yes"), "view.confirm`'s `on_yes")
        on_no = self._station_arg(bound.get("on_no"), "view.confirm`'s `on_no")
        value = bound.get("value", NOTHING)
        self._add(
            UIElement(
                "confirm",
                value=text,
                station=on_yes,
                payload=on_no,
                data={"value": value},
            )
        )
        return NOTHING

    # ------------------------------------------------------------- execution
    def run_source(self, source: str, filename: str = "<browser>") -> None:
        self.last_source = source
        self.last_filename = filename
        self.output = []
        self.diagnostics = []
        self.error = None
        self.view_state = {}
        self._vclock = 0
        self._reset_view()
        compiled = compile_source(source, filename, loader=self.loader)
        self.diagnostics = list(compiled.diagnostics)
        if compiled.has_errors:
            self._changed()
            return
        self.runtime = self._new_runtime()
        result = execute(
            compiled.program,
            filename=filename,
            loader=self.loader,
            hosts=self.hosts,
            host_name="python+browser",
            max_steps=self.max_steps,
            max_depth=self.max_depth,
            on_output=self._emit,
        )
        if result.error is not None:
            self.error = result.error
        self._changed()

    # ------------------------------------------------------------- callbacks
    def _invoke(self, station: Any, args: list[Any]) -> None:
        pairs = [(None, value) for value in args]
        try:
            self.runtime.call_value(station, pairs, None)
            self.error = None
        except MiRuntimeError as err:
            self.error = err

    def press(self, index: int) -> None:
        """Activate the button at ``index`` in the current render."""
        if not (0 <= index < len(self.ui_elements)):
            return
        element = self.ui_elements[index]
        if element.kind != "button" or element.station is None:
            return
        payload = NOTHING if element.payload is _NO_VALUE else element.payload
        event = Assembly.from_pairs(
            [("event", "click"), ("id", element.button_id), ("value", payload)]
        )
        self._invoke(element.station, [event])
        self._changed()

    def confirm(self, index: int, yes: bool) -> None:
        """Answer a ``view.confirm`` prompt at ``index``."""
        if not (0 <= index < len(self.ui_elements)):
            return
        element = self.ui_elements[index]
        if element.kind != "confirm":
            return
        station = element.station if yes else element.payload
        value = element.data.get("value", NOTHING) if isinstance(element.data, dict) else NOTHING
        event = Assembly.from_pairs([("event", "confirm"), ("yes", yes), ("value", value)])
        self._invoke(station, [event])
        self._changed()

    def click_row(self, index: int, row_number: int) -> None:
        """Click row ``row_number`` (1-based) of the table at ``index``."""
        if not (0 <= index < len(self.ui_elements)):
            return
        element = self.ui_elements[index]
        if element.kind != "table" or element.station is None or not isinstance(element.data, dict):
            return
        rows = element.data["rows"]
        if not (1 <= row_number <= len(rows)):
            return
        row = rows[row_number - 1]
        id_field = element.data.get("id_field")
        row_id = row_number
        if id_field:
            columns = element.data["columns"]
            if id_field in columns:
                row_id = row[columns.index(id_field)]
        event = Assembly.from_pairs(
            [
                ("event", "row"),
                ("id", row_id),
                ("value", tuple(row)),
                ("row", row_number),
            ]
        )
        self._invoke(element.station, [event])
        self._changed()

    def click_cell(self, index: int, row: int, col: int) -> None:
        """Click cell (row, col) — 1-based — of the map at ``index``."""
        if not (0 <= index < len(self.ui_elements)):
            return
        element = self.ui_elements[index]
        if element.kind != "map" or element.station is None or not isinstance(element.data, dict):
            return
        cells = element.data["cells"]
        if not (1 <= row <= len(cells)):
            return
        if not (1 <= col <= len(cells[row - 1])):
            return
        code = cells[row - 1][col - 1]
        event = Assembly.from_pairs(
            [("event", "cell"), ("row", row), ("col", col), ("code", code)]
        )
        self._invoke(element.station, [event])
        self._changed()

    # -------------------------------------------------------------- timers
    @property
    def vclock(self) -> int:
        return self._vclock

    def pump(self, ms: int = 0, limit: int = 10_000) -> int:
        """Advance the view clock and fire due timers; returns how many fired."""
        self._vclock += max(0, ms)
        fired = 0
        steps = 0
        while steps < limit:
            steps += 1
            due = [timer for timer in self.timers if timer.due <= self._vclock]
            if not due:
                break
            timer = min(due, key=lambda item: (item.due, item.handle))
            fired += 1
            if timer.repeat:
                timer.due += max(1, timer.interval)
            else:
                self.timers = [item for item in self.timers if item.handle != timer.handle]
            if timer.element >= 0:
                event = Assembly.from_pairs(
                    [
                        ("event", "timer"),
                        ("label", timer.label),
                        ("seconds", timer.seconds),
                        ("value", timer.value),
                    ]
                )
            else:
                event = Assembly.from_pairs(
                    [("event", "timer"), ("label", ""), ("seconds", 0), ("value", timer.value)]
                )
            self._invoke(timer.station, [event])
            if self.error is not None:
                break
        if fired:
            self._changed()
        return fired

    def remaining(self, index: int) -> int:
        """Seconds left on the ``view.timer`` at ``index`` (0 when done)."""
        if not (0 <= index < len(self.ui_elements)):
            return 0
        element = self.ui_elements[index]
        if element.kind != "timer":
            return 0
        for timer in self.timers:
            if timer.element == index:
                return max(0, (timer.due - self._vclock + 999) // 1000)
        return 0

    def set_input(self, label: str, text: str) -> None:
        self.inputs[label] = text

    def button_index(self, label: str) -> int | None:
        for element in self.ui_elements:
            if element.kind == "button" and element.label == label:
                return element.index
        return None

    def press_label(self, label: str) -> bool:
        index = self.button_index(label)
        if index is None:
            return False
        self.press(index)
        return True

    def find(self, kind: str, value: str | None = None) -> int | None:
        for element in self.ui_elements:
            if element.kind == kind and (value is None or element.value == value):
                return element.index
        return None

    # ------------------------------------------------------------- networking
    def open_url(self, url: str) -> bool:
        """Fetch an MI source file over HTTP and run it (the app address bar)."""
        url = url.strip()
        self.address = url
        if not url:
            self.diagnostics = [
                Diagnostic(
                    "error",
                    "E0309",
                    "enter the URL of an MI source file",
                    _dummy_span(),
                )
            ]
            self._changed()
            return False
        try:
            request = urllib.request.Request(
                url, headers={"Accept": "text/plain", "User-Agent": "MiBrowser/1.0"}
            )
            with urllib.request.urlopen(request, timeout=15) as response:
                source = response.read().decode("utf-8", "replace")
        except (urllib.error.URLError, OSError, ValueError) as exc:
            self.diagnostics = [
                Diagnostic(
                    "error",
                    "E0309",
                    f"cannot fetch app source from {url}: {exc}",
                    _dummy_span(),
                    "start an MI backend first, e.g. python -m 米 run 应用/forum/backend.mi",
                )
            ]
            self.error = None
            self._changed()
            return False
        self.run_source(source, url)
        return True

    # --------------------------------------------------------------- helpers
    @property
    def has_errors(self) -> bool:
        return any(diag.severity == "error" for diag in self.diagnostics) or (
            self.error is not None
        )

    def visible_text(self) -> str:
        """Everything currently rendered, as plain text (used by 测试/tools)."""
        lines: list[str] = []
        for element in self.ui_elements:
            kind = element.kind
            if kind in ("heading", "text", "button", "input", "badge", "notice", "confirm"):
                lines.append(element.value if kind != "button" else element.label)
            elif kind in ("screen", "card"):
                lines.append(element.label)
                if element.value:
                    lines.append(element.value)
            elif kind == "list" and isinstance(element.data, dict):
                for item in element.data["items"]:
                    lines.append(item["text"])
                    if item["detail"]:
                        lines.append(item["detail"])
            elif kind == "table" and isinstance(element.data, dict):
                lines.append(" | ".join(element.data["columns"]))
                for row in element.data["rows"]:
                    lines.append(" | ".join(str(cell) for cell in row))
            elif kind == "progress":
                lines.append(f"{element.label} {element.value}")
            elif kind == "map" and isinstance(element.data, dict):
                for row in element.data["cells"]:
                    lines.append(" ".join(row))
            elif kind == "timer":
                lines.append(f"{element.label} {self.remaining(element.index)}")
            elif kind == "image":
                lines.append(element.value)
        return "\n".join(lines)

    def input_labels(self) -> list[str]:
        return [element.label for element in self.ui_elements if element.kind == "input"]

    def button_labels(self) -> list[str]:
        return [element.label for element in self.ui_elements if element.kind == "button"]

    def table_rows(self, index: int) -> list[list[str]]:
        if not (0 <= index < len(self.ui_elements)):
            return []
        element = self.ui_elements[index]
        if element.kind != "table" or not isinstance(element.data, dict):
            return []
        return [[str(cell) for cell in row] for row in element.data["rows"]]
