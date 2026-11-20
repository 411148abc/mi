"""Loading MI modules for `use`.

Modules are parsed and statically checked when first imported; their top-level
flows are never executed.  Import cycles are reported as E0307.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from . import ast
from .checker import check
from .errors import Diagnostic, ModuleError
from .parser import parse
from .tokens import Position, Span


def _dummy_span() -> Span:
    return Span(Position(1, 1, 0), Position(1, 1, 0))


@dataclass
class Module:
    path: str
    name: str
    program: ast.Program
    stations: dict[str, ast.Station] = field(default_factory=dict)
    imports: dict[str, "Module"] = field(default_factory=dict)
    flat: list["Module"] = field(default_factory=list)

    def find_station(self, name: str) -> ast.Station | None:
        station = self.stations.get(name)
        if station is not None:
            return station
        for module in self.flat:
            found = module.stations.get(name)
            if found is not None:
                return found
        return None


VIRTUAL_ORIGINS = {"<source>", "<memory>", "<repl>", "<stdin>", "<string>"}


class ModuleLoader:
    def __init__(self) -> None:
        self.cache: dict[str, Module] = {}
        self.loading: list[str] = []

    # ------------------------------------------------------------- resolving
    def resolve(self, path: str, origin: str | None) -> Path:
        candidate = Path(path)
        if not candidate.is_absolute():
            base = (
                Path(origin).resolve().parent
                if origin and origin not in VIRTUAL_ORIGINS
                else Path.cwd()
            )
            candidate = base / candidate
        if not candidate.suffix:
            candidate = candidate.with_suffix(".mi")
        return candidate.resolve()

    # -------------------------------------------------------------- loading
    def load(self, path: str, origin: str | None = None) -> Module:
        resolved = self.resolve(path, origin)
        key = str(resolved)
        if key in self.cache:
            return self.cache[key]
        if key in self.loading:
            raise ModuleError(
                [
                    Diagnostic(
                        "error",
                        "E0307",
                        f"module cycle detected while loading `{path}`",
                        _dummy_span(),
                        "break the `use` cycle between modules",
                    )
                ]
            )
        if not resolved.exists():
            raise ModuleError(
                [
                    Diagnostic(
                        "error",
                        "E0307",
                        f"cannot find module `{path}`",
                        _dummy_span(),
                        "check the path; `.mi` is added automatically",
                    )
                ]
            )
        try:
            text = resolved.read_text(encoding="utf-8")
        except OSError as exc:  # pragma: no cover - rare
            raise ModuleError(
                [
                    Diagnostic(
                        "error",
                        "E0307",
                        f"cannot read module `{path}`: {exc}",
                        _dummy_span(),
                    )
                ]
            ) from exc

        self.loading.append(key)
        try:
            program, diagnostics = parse(text, str(resolved))
            diagnostics = diagnostics + check(
                program, filename=str(resolved), loader=self.station_params
            )
            if any(d.severity == "error" for d in diagnostics):
                raise ModuleError(diagnostics)
            module = Module(path=str(resolved), name=resolved.stem, program=program)
            for item in program.items:
                if isinstance(item, ast.Station):
                    module.stations[item.name] = item
            for item in program.items:
                if isinstance(item, ast.Use):
                    child = self.load(item.path, str(resolved))
                    if item.alias:
                        module.imports[item.alias] = child
                    else:
                        module.flat.append(child)
            self.cache[key] = module
            return module
        finally:
            self.loading.pop()

    # ------------------------------------------------- checker-facing loader
    def station_params(self, path: str, origin: str | None = None) -> dict[str, list]:
        module = self.load(path, origin)
        return {name: station.params for name, station in module.stations.items()}

    def virtual(self, files: dict[str, str], root: str = ".") -> "VirtualModuleLoader":
        """Build a loader from an in-memory file tree (used by 测试/browser)."""
        return VirtualModuleLoader(files, root)


class VirtualModuleLoader(ModuleLoader):
    """A loader backed by an in-memory ``{path: source}`` map."""

    def __init__(self, files: dict[str, str], root: str = ".") -> None:
        super().__init__()
        self.files = {str(Path(key).as_posix()): value for key, value in files.items()}
        self.root = Path(root).resolve()

    def resolve(self, path: str, origin: str | None) -> Path:
        candidate = Path(path)
        if not candidate.is_absolute():
            base = (
                Path(origin).parent
                if origin and origin not in VIRTUAL_ORIGINS
                else self.root
            )
            candidate = base / candidate
        if not candidate.suffix:
            candidate = candidate.with_suffix(".mi")
        return candidate.resolve()

    def load(self, path: str, origin: str | None = None) -> Module:
        resolved = self.resolve(path, origin)
        key = str(resolved)
        if key in self.cache:
            return self.cache[key]
        if key in self.loading:
            raise ModuleError(
                [
                    Diagnostic(
                        "error",
                        "E0307",
                        f"module cycle detected while loading `{path}`",
                        _dummy_span(),
                    )
                ]
            )
        try:
            relative = resolved.relative_to(self.root).as_posix()
        except ValueError:
            relative = Path(path).as_posix()
        if relative not in self.files:
            raise ModuleError(
                [
                    Diagnostic(
                        "error",
                        "E0307",
                        f"cannot find module `{path}`",
                        _dummy_span(),
                        "this runtime only sees the virtual files given to it",
                    )
                ]
            )
        text = self.files[relative]
        self.loading.append(key)
        try:
            program, diagnostics = parse(text, str(resolved))
            diagnostics = diagnostics + check(
                program, filename=str(resolved), loader=self.station_params
            )
            if any(d.severity == "error" for d in diagnostics):
                raise ModuleError(diagnostics)
            module = Module(path=str(resolved), name=resolved.stem, program=program)
            for item in program.items:
                if isinstance(item, ast.Station):
                    module.stations[item.name] = item
            for item in program.items:
                if isinstance(item, ast.Use):
                    child = self.load(item.path, str(resolved))
                    if item.alias:
                        module.imports[item.alias] = child
                    else:
                        module.flat.append(child)
            self.cache[key] = module
            return module
        finally:
            self.loading.pop()
