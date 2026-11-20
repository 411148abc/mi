"""The MI backend: a deterministic tree-walking evaluator.

Execution model: top-level flows run in order; stations are pure functions with
named input ports and named output ports; the only mutable state is the wire
frame of the scope currently running.  All values are immutable.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, NoReturn

from . import ast
from .builtins import BUILTIN_IMPLS
from .errors import MiRuntimeError, ModuleError, TraceFrame
from .glyphs import TABLE
from .modules import Module, ModuleLoader
from .values import (
    NOTHING,
    Assembly,
    BuiltinValue,
    MiValueError,
    StationValue,
    as_int,
    as_list,
    binary,
    display,
    kind,
    truthy,
    unary,
)


class _HaltSignal(Exception):
    pass


class _SkipSignal(Exception):
    pass


class _ReturnSignal(Exception):
    def __init__(self, value: Any) -> None:
        super().__init__("return")
        self.value = value


_NO_RETURN = object()


@dataclass
class ExecutionOptions:
    max_steps: int = 5_000_000
    max_depth: int = 400


@dataclass
class ExecutionResult:
    output: list[str] = field(default_factory=list)
    error: MiRuntimeError | None = None

    @property
    def ok(self) -> bool:
        return self.error is None


@dataclass
class Frame:
    module: Module
    station: ast.Station | None = None
    params: dict[str, Any] = field(default_factory=dict)
    wires: dict[str, Any] = field(default_factory=dict)
    variables: dict[str, Any] = field(default_factory=dict)
    ports: dict[str, Any] = field(default_factory=dict)


class Runtime:
    def __init__(
        self,
        *,
        loader: ModuleLoader | None = None,
        hosts: dict[str, Any] | None = None,
        host_name: str = "python",
        argv: list[str] | None = None,
        options: ExecutionOptions | None = None,
        on_output: Any = None,
    ) -> None:
        self.loader = loader or ModuleLoader()
        self.hosts = dict(hosts or {})
        self.host_name = host_name
        self.argv = list(argv or [])
        self.options = options or ExecutionOptions()
        self.on_output = on_output
        self.output: list[str] = []
        self.steps = 0
        self.depth = 0
        self.stack: list[TraceFrame] = []
        self.impls: dict[str, Any] = dict(BUILTIN_IMPLS)
        self.impls.update(self.hosts)

    # --------------------------------------------------------------- helpers
    def emit_line(self, text: str) -> None:
        self.output.append(text)
        if self.on_output is not None:
            self.on_output(text)

    def tick(self, span: Any) -> None:
        self.steps += 1
        if self.steps > self.options.max_steps:
            self.fail(
                "E0305",
                f"step budget of {self.options.max_steps} exhausted",
                span,
                "raise it with --max-steps, or make the cycle terminate",
            )

    def fail(
        self,
        code: str,
        message: str,
        span: Any,
        hint: str | None = None,
    ) -> NoReturn:
        raise MiRuntimeError(
            code, message, span, hint=hint, trace=list(self.stack)
        )

    # ------------------------------------------------------------- entry point
    def run(self, program: ast.Program, filename: str = "<source>") -> ExecutionResult:
        import sys

        previous_limit = sys.getrecursionlimit()
        if previous_limit < 20000:
            sys.setrecursionlimit(20000)
        module = Module(path=filename, name="main", program=program)
        frame = Frame(module)
        try:
            self._register(module, program, filename)
            self.exec_items(program.items, frame)
            return ExecutionResult(list(self.output), None)
        except MiRuntimeError as err:
            return ExecutionResult(list(self.output), err)
        except RecursionError:
            return ExecutionResult(
                list(self.output),
                MiRuntimeError(
                    "E0306",
                    "the runtime recursion limit was reached",
                    None,
                    hint="check the recursion in your stations",
                    trace=list(self.stack),
                ),
            )
        except ModuleError as exc:
            diagnostic = exc.diagnostics[0] if exc.diagnostics else None
            if diagnostic is None:  # pragma: no cover
                return ExecutionResult(
                    list(self.output), MiRuntimeError("E0307", "module error")
                )
            return ExecutionResult(
                list(self.output),
                MiRuntimeError(
                    diagnostic.code,
                    diagnostic.message,
                    diagnostic.span,
                    hint=diagnostic.hint,
                ),
            )
        except (_HaltSignal, _SkipSignal, _ReturnSignal):  # pragma: no cover
            return ExecutionResult(list(self.output), None)
        finally:
            sys.setrecursionlimit(previous_limit)

    def _register(self, module: Module, program: ast.Program, origin: str) -> None:
        for item in program.items:
            if isinstance(item, ast.Station):
                module.stations[item.name] = item
        for item in program.items:
            if isinstance(item, ast.Use):
                child = self.loader.load(item.path, origin)
                if item.alias:
                    module.imports[item.alias] = child
                else:
                    module.flat.append(child)

    # ---------------------------------------------------------------- flows
    def exec_items(self, items: list[ast.Node], frame: Frame) -> None:
        for item in items:
            self.exec_flow(item, frame)

    def exec_block(self, block: ast.Block, frame: Frame) -> None:
        for flow in block.flows:
            self.exec_flow(flow, frame)

    def exec_flow(self, flow: ast.Node, frame: Frame) -> None:
        self.tick(getattr(flow, "span", None))
        if isinstance(flow, ast.WireDecl):
            frame.wires[flow.name] = (
                self.eval(flow.init, frame) if flow.init is not None else NOTHING
            )
            return
        if isinstance(flow, ast.Emit):
            if flow.target in frame.params:
                self.fail(
                    "E0202", f"`{flow.target}` is a read-only port", flow.span
                )
            if flow.target not in frame.wires:
                self.fail(
                    "E0200",
                    f"wire `{flow.target}` is used before it is declared",
                    flow.span,
                )
            frame.wires[flow.target] = self.eval(flow.value, frame)
            return
        if isinstance(flow, ast.When):
            self.tick(flow.span)
            if truthy(self.eval(flow.cond, frame)):
                self.exec_block(flow.body, frame)
            elif isinstance(flow.otherwise, ast.Block):
                self.exec_block(flow.otherwise, frame)
            elif isinstance(flow.otherwise, ast.When):
                self.exec_flow(flow.otherwise, frame)
            return
        if isinstance(flow, ast.Cycle):
            self.exec_cycle(flow, frame)
            return
        if isinstance(flow, ast.For):
            try:
                items = as_list(self.eval(flow.items, frame), "`for`'s list")
            except MiValueError as exc:
                self.fail(exc.code, exc.message, flow.span)
            for index, item in enumerate(items):
                frame.variables[flow.var] = item
                self.tick(flow.span)
                try:
                    self.exec_block(flow.body, frame)
                except _SkipSignal:
                    continue
                except _HaltSignal:
                    break
            return
        if isinstance(flow, ast.Out):
            if frame.station is None:
                self.fail("E0206", "`out` is only allowed inside a station", flow.span)
            frame.ports[flow.name] = self.eval(flow.value, frame)
            return
        if isinstance(flow, ast.Return):
            value = (
                self.eval(flow.value, frame) if flow.value is not None else _NO_RETURN
            )
            raise _ReturnSignal(value)
        if isinstance(flow, ast.Halt):
            raise _HaltSignal()
        if isinstance(flow, ast.Skip):
            raise _SkipSignal()
        if isinstance(flow, ast.Flow):
            self.eval(flow.expr, frame)
            return
        if isinstance(flow, (ast.Station, ast.Use)):  # pragma: no cover - handled earlier
            return

    def exec_cycle(self, cycle: ast.Cycle, frame: Frame) -> None:
        kind = cycle.kind
        if kind == "count":
            raw = self.eval(cycle.head, frame) if cycle.head is not None else 0
            try:
                count = as_int(raw, "the cycle count")
            except MiValueError as exc:
                self.fail(exc.code, exc.message, cycle.span)
            if count < 0:
                self.fail("E0300", "the cycle count cannot be negative", cycle.span)
            for index in range(count):
                if cycle.var:
                    frame.variables[cycle.var] = index
                self.tick(cycle.span)
                try:
                    self.exec_block(cycle.body, frame)
                except _SkipSignal:
                    continue
                except _HaltSignal:
                    break
            return
        if kind in ("while", "until"):
            index = 0
            while True:
                condition = truthy(self.eval(cycle.head, frame))
                if kind == "while" and not condition:
                    break
                if kind == "until" and condition:
                    break
                if cycle.var:
                    frame.variables[cycle.var] = index
                self.tick(cycle.span)
                try:
                    self.exec_block(cycle.body, frame)
                except _SkipSignal:
                    index += 1
                    continue
                except _HaltSignal:
                    break
                index += 1
            return
        if kind == "over":
            try:
                values = as_list(self.eval(cycle.head, frame), "`cycle over`'s list")
            except MiValueError as exc:
                self.fail(exc.code, exc.message, cycle.span)
            for item in values:
                if cycle.var:
                    frame.variables[cycle.var] = item
                self.tick(cycle.span)
                try:
                    self.exec_block(cycle.body, frame)
                except _SkipSignal:
                    continue
                except _HaltSignal:
                    break
            return
        self.fail("E0300", f"unknown cycle kind `{kind}`", cycle.span)  # pragma: no cover

    # ----------------------------------------------------------- expressions
    def eval(self, expr: ast.Expr, frame: Frame) -> Any:
        self.tick(getattr(expr, "span", None))
        if isinstance(expr, ast.NumberLit):
            return expr.value
        if isinstance(expr, ast.TextLit):
            return expr.value
        if isinstance(expr, ast.FlagLit):
            return expr.value
        if isinstance(expr, ast.NilLit):
            return NOTHING
        if isinstance(expr, ast.ListLit):
            return tuple(self.eval(item, frame) for item in expr.items)
        if isinstance(expr, ast.AssemblyLit):
            return Assembly.from_pairs(
                [(field.name, self.eval(field.value, frame)) for field in expr.fields]
            )
        if isinstance(expr, ast.Name):
            return self.eval_name(expr, frame)
        if isinstance(expr, ast.Read):
            return self.eval_read(expr, frame)
        if isinstance(expr, ast.Field):
            base = self.eval(expr.base, frame)
            if isinstance(base, Assembly):
                try:
                    return base.get(expr.name)
                except KeyError:
                    self.fail(
                        "E0302",
                        f"this assembly has no field `{expr.name}`",
                        expr.span,
                        "fields: " + (", ".join(base.keys()) or "none"),
                    )
            self.fail(
                "E0300",
                f"cannot read field `{expr.name}` of {kind(base)}",
                expr.span,
            )
        if isinstance(expr, ast.Unary):
            try:
                return unary(expr.op, self.eval(expr.operand, frame))
            except MiValueError as exc:
                self.fail(exc.code, exc.message, expr.span)
        if isinstance(expr, ast.Binary):
            left = self.eval(expr.left, frame)
            if expr.op == "AND":
                return self.eval(expr.right, frame) if truthy(left) else left
            if expr.op == "OR":
                return left if truthy(left) else self.eval(expr.right, frame)
            right = self.eval(expr.right, frame)
            try:
                return binary(expr.op, left, right)
            except MiValueError as exc:
                self.fail(exc.code, exc.message, expr.span)
        if isinstance(expr, ast.Call):
            return self.eval_call(expr, frame)
        self.fail("E0300", "cannot evaluate this expression", getattr(expr, "span", None))

    def eval_name(self, node: ast.Name, frame: Frame) -> Any:
        name = node.name
        if node.glyph:
            builtin = TABLE.builtin(name)
            if builtin is None:  # pragma: no cover - parser only emits known glyphs
                self.fail("E0203", f"unknown builtin `{name}`", node.span)
            return BuiltinValue(builtin.canonical)
        if name in frame.params:
            return frame.params[name]
        if name in frame.wires:
            return frame.wires[name]
        if name in frame.variables:
            return frame.variables[name]
        station = frame.module.find_station(name)
        if station is not None:
            module = frame.module
            if name not in module.stations:
                for candidate in module.flat:
                    if name in candidate.stations:
                        module = candidate
                        break
            return StationValue(module, station)
        builtin = TABLE.builtin(name)
        if builtin is not None:
            return BuiltinValue(builtin.canonical)
        self.fail("E0203", f"unknown name `{name}`", node.span)

    def eval_read(self, node: ast.Read, frame: Frame) -> Any:
        if node.name in frame.params:
            return frame.params[node.name]
        if node.name in frame.wires:
            return frame.wires[node.name]
        if node.name in frame.variables:
            return frame.variables[node.name]
        self.fail(
            "E0200",
            f"`draw` needs a declared wire, but `{node.name}` is not one",
            node.span,
            "declare it with `wire`",
        )

    # ---------------------------------------------------------------- calls
    def eval_call(self, call: ast.Call, frame: Frame) -> Any:
        pairs = [(arg.label, self.eval(arg.value, frame)) for arg in call.args]
        name = call.name
        if name is None:
            callee = self.eval(call.callee, frame)
            return self.call_value(callee, pairs, call.span)
        if call.glyph:
            builtin = TABLE.builtin(name)
            if builtin is None:  # pragma: no cover
                self.fail("E0203", f"unknown builtin `{name}`", call.span)
            return self.call_builtin(builtin.canonical, pairs, call.span)
        if "." in name:
            prefix, _, rest = name.partition(".")
            module = frame.module.imports.get(prefix)
            if module is not None and rest in module.stations:
                return self.call_station(module, module.stations[rest], pairs, call.span)
            builtin = TABLE.builtin(name)
            if builtin is not None:
                return self.call_builtin(builtin.canonical, pairs, call.span)
            self.fail(
                "E0203",
                f"unknown name `{name}`",
                call.span,
                "import the module with `use … as` first",
            )
        if name in frame.params:
            return self.call_value(frame.params[name], pairs, call.span)
        if name in frame.wires:
            return self.call_value(frame.wires[name], pairs, call.span)
        if name in frame.variables:
            return self.call_value(frame.variables[name], pairs, call.span)
        station = frame.module.find_station(name)
        if station is not None:
            module = frame.module
            if name not in module.stations:
                for candidate in module.flat:
                    if name in candidate.stations:
                        module = candidate
                        break
            return self.call_station(module, station, pairs, call.span)
        builtin = TABLE.builtin(name)
        if builtin is not None:
            return self.call_builtin(builtin.canonical, pairs, call.span)
        self.fail("E0203", f"unknown station or builtin `{name}`", call.span)

    def call_value(self, value: Any, pairs: list[tuple[str | None, Any]], span: Any) -> Any:
        if isinstance(value, StationValue):
            return self.call_station(value.module, value.station, pairs, span)
        if isinstance(value, BuiltinValue):
            return self.call_builtin(value.name, pairs, span)
        self.fail(
            "E0303",
            f"this {kind(value)} is not callable",
            span,
            "put a station or a builtin here",
        )

    def call_builtin(
        self, name: str, pairs: list[tuple[str | None, Any]], span: Any
    ) -> Any:
        builtin = TABLE.builtin(name)
        if builtin is None:
            self.fail("E0203", f"unknown builtin `{name}`", span)
        available = bool(set(self.host_name.split("+")) & set(builtin.hosts))
        if not available:
            self.fail(
                "E0308",
                f"builtin `{builtin.canonical}` is not available on the {self.host_name} host",
                span,
                "hosts: " + ", ".join(builtin.hosts),
            )
        impl = self.impls.get(builtin.canonical)
        if impl is None:
            self.fail(
                "E0308",
                f"builtin `{builtin.canonical}` has no implementation on this host",
                span,
            )
        bound = self._bind_builtin(builtin, pairs, span)
        try:
            return impl(self, bound, span)
        except MiValueError as exc:
            self.fail(exc.code, exc.message, span)

    def call_station(
        self,
        module: Module,
        station: ast.Station,
        pairs: list[tuple[str | None, Any]],
        call_span: Any,
    ) -> Any:
        self.depth += 1
        if self.depth > self.options.max_depth:
            self.depth -= 1
            self.fail(
                "E0306",
                f"call depth limit of {self.options.max_depth} reached",
                call_span,
                "check the recursion in `" + station.name + "`",
            )
        frame = Frame(module, station)
        self.stack.append(TraceFrame(station.name, call_span))
        try:
            bound = self._bind_station(station, pairs, call_span)
            frame.params.update(bound)
            for param in station.params:
                if param.name in frame.params:
                    continue
                if param.default is None:
                    self.fail(
                        "E0205",
                        f"missing argument `{param.name}`",
                        call_span,
                    )
                frame.params[param.name] = self.eval(param.default, frame)
            try:
                self.exec_block(station.body, frame)
            except _ReturnSignal as signal:
                if signal.value is not _NO_RETURN:
                    frame.ports["value"] = signal.value
            except _HaltSignal:
                pass
            return self._station_result(station, frame.ports)
        finally:
            self.stack.pop()
            self.depth -= 1

    def _station_result(self, station: ast.Station, ports: dict[str, Any]) -> Any:
        names = station.out_ports
        if not names:
            return NOTHING
        if len(names) == 1:
            return ports.get(names[0], NOTHING)
        return Assembly.from_pairs(
            [(name, ports[name]) for name in names if name in ports]
        )

    # -------------------------------------------------------------- binding
    @staticmethod
    def _station_param_names(station: ast.Station) -> list[str]:
        return [param.name for param in station.params]

    def _bind_station(
        self, station: ast.Station, pairs: list[tuple[str | None, Any]], span: Any
    ) -> dict[str, Any]:
        names = self._station_param_names(station)
        bound: dict[str, Any] = {}
        next_positional = 0
        for label, value in pairs:
            if label is None:
                if next_positional < len(names):
                    bound[names[next_positional]] = value
                    next_positional += 1
                else:
                    self.fail(
                        "E0205",
                        f"too many arguments for station `{station.name}`",
                        span,
                        f"ports: {', '.join(names) or 'none'}",
                    )
            else:
                if label not in names:
                    self.fail(
                        "E0204",
                        f"unknown argument label `{label}`",
                        span,
                        f"ports: {', '.join(names) or 'none'}",
                    )
                if label in bound:
                    self.fail(
                        "E0204", f"argument `{label}` is given twice", span
                    )
                bound[label] = value
        return bound

    def _bind_builtin(
        self, builtin: Any, pairs: list[tuple[str | None, Any]], span: Any
    ) -> dict[str, Any]:
        names = [param.name for param in builtin.params]
        variadic = builtin.variadic.name if builtin.variadic is not None else None
        bound: dict[str, Any] = {}
        variadic_values: list[Any] = []
        next_positional = 0
        for label, value in pairs:
            if label is None:
                if variadic is not None and next_positional >= len(names) - 1:
                    variadic_values.append(value)
                elif next_positional < len(names):
                    bound[names[next_positional]] = value
                    next_positional += 1
                else:
                    self.fail(
                        "E0205",
                        f"too many arguments for builtin `{builtin.canonical}`",
                        span,
                    )
            else:
                if label not in names:
                    self.fail(
                        "E0204",
                        f"unknown argument label `{label}`",
                        span,
                        f"arguments: {', '.join(names) or 'none'}",
                    )
                if label == variadic:
                    variadic_values.append(value)
                elif label in bound:
                    self.fail("E0204", f"argument `{label}` is given twice", span)
                else:
                    bound[label] = value
        if variadic is not None:
            bound[variadic] = variadic_values
        for param in builtin.params:
            if param.optional or param.variadic:
                continue
            if param.name not in bound:
                self.fail(
                    "E0205",
                    f"missing argument `{param.name}`",
                    span,
                    f"arguments: {', '.join(names)}",
                )
        return bound


def execute(
    program: ast.Program,
    *,
    filename: str = "<source>",
    loader: ModuleLoader | None = None,
    hosts: dict[str, Any] | None = None,
    host_name: str = "python",
    argv: list[str] | None = None,
    max_steps: int = 5_000_000,
    max_depth: int = 400,
    on_output: Any = None,
) -> ExecutionResult:
    options = ExecutionOptions(max_steps=max_steps, max_depth=max_depth)
    runtime = Runtime(
        loader=loader,
        hosts=hosts,
        host_name=host_name,
        argv=argv,
        options=options,
        on_output=on_output,
    )
    return runtime.run(program, filename)
