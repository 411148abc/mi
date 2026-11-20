"""Static checking for MI.

Walks the AST and reports what can be known before running: undeclared wires,
reads of parameters, unknown stations/builtins, argument-label and arity
mistakes, misplaces flows, plus a few warnings.  It never executes code.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable

from . import ast
from .ast import (
    AssemblyLit,
    Binary,
    Block,
    Call,
    Cycle,
    Emit,
    Field,
    FlagLit,
    Flow,
    For,
    Halt,
    ListLit,
    Name,
    NilLit,
    NumberLit,
    Out,
    Param,
    Program,
    Read,
    Return,
    Skip,
    Station,
    TextLit,
    Unary,
    Use,
    When,
    WireDecl,
    walk_flows,
)
from .errors import Diagnostic, DiagnosticBag, suggest
from .glyphs import TABLE, Builtin, GlyphTable
from .tokens import Span

Loader = Callable[[str, str], "dict[str, list[Param]]"]


@dataclass
class Scope:
    wires: set[str] = field(default_factory=set)
    params: set[str] = field(default_factory=set)
    variables: set[str] = field(default_factory=set)
    aliases: set[str] = field(default_factory=set)
    allow_unknown_stations: bool = False

    def clone(self) -> "Scope":
        return Scope(
            wires=set(self.wires),
            params=set(self.params),
            variables=set(self.variables),
            aliases=set(self.aliases),
            allow_unknown_stations=self.allow_unknown_stations,
        )

    def has_value(self, name: str) -> bool:
        return name in self.wires or name in self.params or name in self.variables


@dataclass
class _FunctionUsage:
    wires: list[tuple[str, Span]] = field(default_factory=list)
    reads: set[str] = field(default_factory=set)


class Checker:
    def __init__(
        self,
        program: Program,
        *,
        filename: str = "<source>",
        loader: Loader | None = None,
        table: GlyphTable = TABLE,
    ) -> None:
        self.program = program
        self.filename = filename
        if loader is not None and not callable(loader):
            loader = getattr(loader, "station_params", None)
        self.loader = loader
        self.table = table
        self.bag = DiagnosticBag(filename=filename)
        self.stations: dict[str, Station] = {}
        self.aliases: set[str] = set()
        self.flat_import = False
        self.usage = _FunctionUsage()

    # ------------------------------------------------------------------ main
    def check(self) -> list[Diagnostic]:
        self._collect_top_level()
        root = Scope(aliases=set(self.aliases), allow_unknown_stations=self.flat_import)
        top_items = [
            item
            for item in self.program.items
            if not isinstance(item, (Station, Use))
        ]
        self._check_block_items(top_items, root, in_station=False, cycle_depth=0)
        for name, span in self.usage.wires:
            if name not in self.usage.reads:
                self.bag.warning(
                    "W0100",
                    f"wire `{name}` is declared but never read",
                    span,
                    "remove it, or read it with `draw`",
                )
        return self.bag.sorted()

    # ------------------------------------------------------------- collecting
    def _collect_top_level(self) -> None:
        for item in self.program.items:
            if isinstance(item, Station):
                if item.name in self.stations:
                    self.bag.error(
                        "E0201",
                        f"station `{item.name}` is declared twice",
                        item.span,
                    )
                    continue
                self.stations[item.name] = item
        for item in self.program.items:
            if isinstance(item, Use):
                self._handle_use(item)
        for item in self.program.items:
            if isinstance(item, Station):
                self._check_station(item)
        # re-collect top-level wires sequentially while walking; reset usage
        self.usage = _FunctionUsage()

    def _handle_use(self, item: Use) -> None:
        if item.alias:
            self.aliases.add(item.alias)
        else:
            self.flat_import = True
        if self.loader is None:
            return
        try:
            names = self.loader(item.path, self.filename)
        except Exception as exc:  # ModuleError carries diagnostics
            diagnostics = getattr(exc, "diagnostics", None)
            if diagnostics:
                self.bag.extend(diagnostics)
            else:  # pragma: no cover - defensive
                self.bag.error("E0307", f"cannot load module `{item.path}`", item.span)
            return
        for name in names:
            if item.alias:
                # an aliased import is only reachable as `alias.name`, so it can
                # never collide with a local station.
                continue
            if name in self.stations:
                self.bag.error(
                    "E0201",
                    f"station `{name}` from `{item.path}` collides with a local station",
                    item.span,
                    "import the module with `as` and call `alias.name(...)`",
                )

    # --------------------------------------------------------------- stations
    def _check_station(self, station: Station) -> None:
        saved_usage = self.usage
        self.usage = _FunctionUsage()
        scope = Scope(aliases=set(self.aliases), allow_unknown_stations=self.flat_import)
        seen_params: set[str] = set()
        for param in station.params:
            if param.name in seen_params:
                self.bag.error(
                    "E0201", f"parameter `{param.name}` is declared twice", param.span
                )
            seen_params.add(param.name)
            if param.default is not None:
                self._check_expr(param.default, scope)
            scope.params.add(param.name)
        self._check_block_items(
            station.body.flows, scope, in_station=True, cycle_depth=0
        )
        for name, span in self.usage.wires:
            if name not in self.usage.reads:
                self.bag.warning(
                    "W0100",
                    f"wire `{name}` is declared but never read",
                    span,
                    "remove it, or read it with `draw`",
                )
        self.usage = saved_usage

    # ------------------------------------------------------------------ items
    def _check_block_items(
        self,
        items: list[ast.Node],
        scope: Scope,
        *,
        in_station: bool,
        cycle_depth: int,
    ) -> None:
        terminated = False
        for item in items:
            if terminated:
                self.bag.warning(
                    "W0103", "this flow can never run", getattr(item, "span", self.program.span)
                )
            self._check_flow(item, scope, in_station=in_station, cycle_depth=cycle_depth)
            if isinstance(item, (Halt, Return)):
                terminated = True

    def _check_flow(
        self,
        flow: ast.Node,
        scope: Scope,
        *,
        in_station: bool,
        cycle_depth: int,
        station: Station | None = None,
    ) -> None:
        if isinstance(flow, WireDecl):
            if flow.init is not None:
                self._check_expr(flow.init, scope)
            if flow.name in scope.params or flow.name in scope.variables:
                self.bag.error(
                    "E0202",
                    f"`{flow.name}` is a read-only port and cannot be redeclared as a wire",
                    flow.span,
                )
            elif flow.name in scope.wires:
                self.bag.error(
                    "E0201", f"wire `{flow.name}` is declared twice", flow.span
                )
            else:
                if self._builtin(flow.name) is not None:
                    self.bag.warning(
                        "W0102",
                        f"wire `{flow.name}` shadows the builtin of the same name",
                        flow.span,
                        "pick another name, or call the builtin before shadowing",
                    )
                scope.wires.add(flow.name)
                self.usage.wires.append((flow.name, flow.span))
        elif isinstance(flow, Emit):
            if flow.target in scope.params or flow.target in scope.variables:
                self.bag.error(
                    "E0202",
                    f"`{flow.target}` is a read-only port and cannot be written",
                    flow.span,
                    "declare a local wire with `wire` and emit into that",
                )
            elif flow.target not in scope.wires:
                close = suggest(flow.target, sorted(scope.wires), n=1)
                hint = f"did you mean `{close[0]}`?" if close else "declare it with `wire`"
                self.bag.error(
                    "E0200",
                    f"wire `{flow.target}` is used before it is declared",
                    flow.span,
                    hint,
                )
            self._check_expr(flow.value, scope)
        elif isinstance(flow, When):
            self._check_expr(flow.cond, scope)
            inner = scope.clone()
            self._check_block_items(
                flow.body.flows, inner, in_station=in_station, cycle_depth=cycle_depth
            )
            if isinstance(flow.otherwise, Block):
                self._check_block_items(
                    flow.otherwise.flows,
                    scope.clone(),
                    in_station=in_station,
                    cycle_depth=cycle_depth,
                )
            elif isinstance(flow.otherwise, When):
                self._check_flow(
                    flow.otherwise,
                    scope.clone(),
                    in_station=in_station,
                    cycle_depth=cycle_depth,
                )
        elif isinstance(flow, Cycle):
            if flow.head is not None:
                self._check_expr(flow.head, scope)
            inner = scope.clone()
            if flow.var:
                inner.variables.add(flow.var)
            self._check_block_items(
                flow.body.flows, inner, in_station=in_station, cycle_depth=cycle_depth + 1
            )
            if flow.kind in ("while", "until") and not _has_exit(flow.body):
                self.bag.warning(
                    "W0101",
                    "this cycle has no `halt` and may never finish",
                    flow.span,
                    "add a `halt`, or iterate with `cycle over`",
                )
        elif isinstance(flow, For):
            self._check_expr(flow.items, scope)
            inner = scope.clone()
            inner.variables.add(flow.var)
            self._check_block_items(
                flow.body.flows, inner, in_station=in_station, cycle_depth=cycle_depth + 1
            )
        elif isinstance(flow, Out):
            if not in_station:
                self.bag.error(
                    "E0206", "`out` is only allowed inside a station", flow.span
                )
            else:
                self._check_expr(flow.value, scope)
        elif isinstance(flow, Return):
            if not in_station:
                self.bag.error(
                    "E0206", "`return` is only allowed inside a station", flow.span
                )
            if flow.value is not None:
                self._check_expr(flow.value, scope)
        elif isinstance(flow, Halt):
            pass
        elif isinstance(flow, Skip):
            if cycle_depth == 0:
                self.bag.error(
                    "E0304",
                    "`skip` can only be used inside a cycle",
                    flow.span,
                    "use `halt` to leave a station",
                )
        elif isinstance(flow, Station):
            self.bag.error(
                "E0206",
                "stations must be declared at the top level",
                flow.span,
                "move it out of the block",
            )
            self._check_station(flow)
        elif isinstance(flow, Use):
            self.bag.error(
                "E0206", "`use` is only allowed at the top level", flow.span
            )
        elif isinstance(flow, Flow):
            self._check_expr(flow.expr, scope)
        else:  # pragma: no cover - defensive
            pass

    # ------------------------------------------------------------- expressions
    def _check_expr(self, expr: ast.Expr, scope: Scope) -> None:
        if isinstance(expr, (NumberLit, TextLit, FlagLit, NilLit)):
            return
        if isinstance(expr, ListLit):
            for item in expr.items:
                self._check_expr(item, scope)
            return
        if isinstance(expr, AssemblyLit):
            for field in expr.fields:
                self._check_expr(field.value, scope)
            return
        if isinstance(expr, Name):
            self._check_value_reference(expr.name, expr, scope, expr.glyph)
            return
        if isinstance(expr, Read):
            if expr.name in scope.params or expr.name in scope.variables:
                self.usage.reads.add(expr.name)
                return
            if expr.name in scope.wires:
                self.usage.reads.add(expr.name)
                return
            close = suggest(expr.name, sorted(scope.wires), n=1)
            hint = f"did you mean `{close[0]}`?" if close else "declare it with `wire` first"
            self.bag.error(
                "E0200",
                f"`draw` needs a declared wire, but `{expr.name}` is not one",
                expr.span,
                hint,
            )
            return
        if isinstance(expr, Field):
            self._check_expr(expr.base, scope)
            return
        if isinstance(expr, Unary):
            self._check_expr(expr.operand, scope)
            return
        if isinstance(expr, Binary):
            self._check_expr(expr.left, scope)
            self._check_expr(expr.right, scope)
            return
        if isinstance(expr, Call):
            self._check_call(expr, scope)
            return

    def _check_value_reference(
        self, name: str, node: ast.Expr, scope: Scope, glyph: bool = False
    ) -> None:
        if glyph:
            if self.table.builtin(name) is None:
                self.bag.error(
                    "E0203", f"unknown builtin glyph `{name}`", node.span
                )
            return
        if scope.has_value(name):
            if name in scope.wires:
                self.usage.reads.add(name)
            return
        if "." in name:
            prefix, _, rest = name.partition(".")
            if prefix in scope.aliases:
                return
            if self.table.builtin(name) is not None:
                return
            self.bag.error(
                "E0203", f"unknown name `{name}`", node.span,
                "import the module with `use … as` first",
            )
            return
        if name in self.stations:
            return
        if self._builtin(name) is not None:
            return
        if scope.allow_unknown_stations:
            return
        self._unknown_name(
            name,
            node.span,
            list(scope.wires)
            + list(scope.params)
            + list(scope.variables)
            + list(self.stations)
            + [b.canonical for b in self.table.builtins],
            what="name",
        )

    def _keyword_suggestion(self, name: str) -> tuple[str, str] | None:
        matches = suggest(name, self.table.keyword_words, n=1, cutoff=0.72)
        if not matches:
            return None
        word = matches[0]
        token_name = self.table.is_keyword_spelling(word)
        keyword = self.table.keyword(token_name) if token_name else None
        aliases = " | ".join(keyword.spellings) if keyword else word
        return word, aliases

    def _unknown_name(
        self, name: str, span: Span, known: list[str], *, what: str = "name"
    ) -> None:
        close = suggest(name, known, n=2, cutoff=0.6)
        if close:
            hint = "maybe you meant " + " or ".join(f"`{c}`" for c in close) + "?"
            self.bag.error("E0203", f"unknown {what} `{name}`", span, hint)
            return
        keyword = self._keyword_suggestion(name)
        if keyword is not None:
            word, aliases = keyword
            self.bag.error(
                "E0101",
                f"unknown keyword `{name}`",
                span,
                f"maybe you meant `{word}` (also written: {aliases})",
            )
            return
        self.bag.error("E0203", f"unknown {what} `{name}`", span)

    def _check_arg_values(self, call: Call, scope: Scope) -> None:
        for arg in call.args:
            self._check_expr(arg.value, scope)

    def _check_call(self, call: Call, scope: Scope) -> None:
        builtin: Builtin | None = None
        name = call.name
        if call.glyph and name is not None:
            builtin = self.table.builtin(name)
            if builtin is None:  # pragma: no cover - parser only emits known glyphs
                self.bag.error("E0203", f"unknown builtin `{name}`", call.span)
                self._check_arg_values(call, scope)
                return
            self._check_expr_args(call, list(builtin.params), call.span, scope)
            return
        if name is None:
            if call.callee is not None:
                self._check_expr(call.callee, scope)
            self._check_arg_values(call, scope)
            return
        if "." in name:
            prefix = name.partition(".")[0]
            if prefix in scope.aliases:
                self._check_arg_values(call, scope)
                return
            builtin = self.table.builtin(name)
            if builtin is None:
                self.bag.error(
                    "E0203",
                    f"unknown name `{name}`",
                    call.span,
                    "module aliases must be imported with `use … as`",
                )
                self._check_arg_values(call, scope)
                return
            self._check_expr_args(call, list(builtin.params), call.span, scope)
            return
        if scope.has_value(name):
            # calling a value: a dynamic call, labels cannot be checked statically
            self._check_arg_values(call, scope)
            return
        station = self.stations.get(name)
        if station is not None:
            self._check_expr_args(call, list(station.params), call.span, scope)
            return
        builtin = self._builtin(name)
        if builtin is not None:
            self._check_expr_args(call, list(builtin.params), call.span, scope)
            return
        if scope.allow_unknown_stations:
            self._check_arg_values(call, scope)
            return
        if call.keyword_typo:
            # the parser already reported E0101 with a keyword suggestion
            self._check_arg_values(call, scope)
            return
        close = suggest(
            name,
            list(self.stations)
            + [b.canonical for b in self.table.builtins]
            + list(scope.wires)
            + list(scope.params)
            + list(scope.variables),
            n=2,
        )
        hint = (
            "maybe you meant " + " or ".join(f"`{c}`" for c in close) + "?"
            if close
            else None
        )
        self.bag.error(
            "E0203", f"unknown station or builtin `{name}`", call.span, hint
        )
        self._check_arg_values(call, scope)

    def _check_expr_args(
        self, call: Call, params: list, span: Span, scope: Scope
    ) -> None:
        names: list[str] = []
        optional: set[str] = set()
        variadic: str | None = None
        for param in params:
            if isinstance(param, Param):
                names.append(param.name)
                if param.default is not None:
                    optional.add(param.name)
            else:  # glyphs.Param
                names.append(param.name)
                if param.optional:
                    optional.add(param.name)
                if param.variadic:
                    variadic = param.name
        positional: list[str] = []
        labelled: dict[str, Span] = {}
        overflow = False
        label_error = False
        for arg in call.args:
            self._check_expr(arg.value, scope)
            if arg.label is None:
                if len(positional) >= len(names):
                    if variadic is None:
                        overflow = True
                else:
                    positional.append(names[len(positional)])
            else:
                if arg.label not in names:
                    if variadic is not None:
                        continue
                    close = suggest(arg.label, names, n=1)
                    hint = (
                        f"did you mean `{close[0]}`?"
                        if close
                        else f"parameters: {', '.join(names) or 'none'}"
                    )
                    self.bag.error(
                        "E0204",
                        f"unknown argument label `{arg.label}`",
                        arg.span,
                        hint,
                    )
                    label_error = True
                elif arg.label in labelled:
                    self.bag.error(
                        "E0204",
                        f"argument `{arg.label}` is given twice",
                        arg.span,
                    )
                else:
                    labelled[arg.label] = arg.span
        if overflow:
            self.bag.error(
                "E0205",
                "too many arguments",
                span,
                f"this station/builtin takes {len(names)} port(s)",
            )
        provided = set(positional) | set(labelled)
        if label_error:
            return
        for required in names:
            if required in optional or required == variadic:
                continue
            if required not in provided:
                self.bag.error(
                    "E0205",
                    f"missing argument `{required}`",
                    span,
                    f"arguments: {', '.join(names)}",
                )

    def _builtin(self, name: str) -> Builtin | None:
        return self.table.builtin(name)


def _has_exit(block: Block) -> bool:
    for flow in walk_flows(block):
        if isinstance(flow, (Halt, Return)):
            return True
        if isinstance(flow, Cycle):
            if any(isinstance(inner, Halt) for inner in walk_flows(flow.body)):
                return True
    return False


def check(
    program: Program,
    *,
    filename: str = "<source>",
    loader: Loader | None = None,
    table: GlyphTable = TABLE,
) -> list[Diagnostic]:
    return Checker(program, filename=filename, loader=loader, table=table).check()
