"""Self-hosted MI runtime — the interpreter written in MI itself.

``米/自举/*.mi`` contains a complete MI interpreter (lexer, parser, evaluator,
builtins, diagnostics) written in MI.  This package is the thin Python bridge
that drives it, so the CLI can offer ``--runtime selfhost`` and the conformance
suite can compare both runtimes.

Files
-----
``tables.mi``   spelling tables baked from ``规格/glyphs.json`` (generated)
``lexer.mi``    source text → tokens
``parser.mi``   tokens → syntax tree
``values.mi``   value model, display, operators
``builtins.mi`` pure builtins (text / list / math / json)
``json.mi``     JSON encoder + decoder
``diag.mi``     static diagnostics (E01xx / E02xx / E0301 / E0303 / E0304)
``eval.mi``     evaluator + ``run_source``
"""
from __future__ import annotations

from .runtime import SelfHostOutcome, run, runtime_files, selfhost_available

__all__ = ["SelfHostOutcome", "run", "runtime_files", "selfhost_available"]
