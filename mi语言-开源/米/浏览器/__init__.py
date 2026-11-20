"""Mi Browser — the Python edition.

* :class:`BrowserController` — headless core: runs MI on the Python runtime,
  implements the ``ui.*`` host as a rendered element model and opens apps over
  HTTP.
* :mod:`米.浏览器.tk_app` — the Tkinter shell (``python -m 米.浏览器``).
* :func:`run_selftest` — the shared conformance suite, headlessly.
"""
from __future__ import annotations

from .conformance import SelfTestResult, run_selftest
from .controller import BrowserController, UIElement

__all__ = ["BrowserController", "UIElement", "SelfTestResult", "run_selftest"]
