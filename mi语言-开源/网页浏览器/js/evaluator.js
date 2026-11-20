/* MI browser runtime -- evaluator, values and public API.
 *
 * Classic script.  Defines:
 *   MI.execute(source, {onOutput, files, hosts, uiHost, ...})
 *       -> {output: string[], diagnostics: Diagnostic[]}
 *   MI.compile(source, {...}) -> {program, diagnostics}
 *   MI.run(program, {...})   -> {output, diagnostics}
 *
 * Port of 米/values.py, 米/modules.py, 米/evaluator.py and 米/api.py.
 */
(function () {
  "use strict";

  window.MI = window.MI || {};
  var MI = window.MI;

  var internals = MI._internals || (MI._internals = {});

  // ================================================================ values
  var NOTHING = { type: "nothing" };

  function diag(severity, code, message, span, hint) {
    return { severity: severity, code: code, message: message, span: span, hint: hint || null };
  }

  function MiValueError(message, code) {
    this.name = "MiValueError";
    this.message = message;
    this.code = code || "E0300";
  }
  MiValueError.prototype = Object.create(Error.prototype);
  MiValueError.prototype.constructor = MiValueError;

  function Assembly(fields) {
    this.type = "assembly";
    this.fields = fields || [];
  }

  Assembly.fromPairs = function (pairs) {
    var ordered = [];
    var index = {};
    for (var i = 0; i < pairs.length; i += 1) {
      var name = pairs[i][0];
      var value = pairs[i][1];
      if (Object.prototype.hasOwnProperty.call(index, name)) {
        ordered[index[name]] = [name, value];
      } else {
        index[name] = ordered.length;
        ordered.push([name, value]);
      }
    }
    return new Assembly(ordered);
  };

  Assembly.prototype.get = function (name) {
    for (var i = 0; i < this.fields.length; i += 1) {
      if (this.fields[i][0] === name) return this.fields[i][1];
    }
    throw new MiValueError("this assembly has no field `" + name + "`", "E0302");
  };

  Assembly.prototype.has = function (name) {
    for (var i = 0; i < this.fields.length; i += 1) {
      if (this.fields[i][0] === name) return true;
    }
    return false;
  };

  Assembly.prototype.keys = function () {
    var out = [];
    for (var i = 0; i < this.fields.length; i += 1) out.push(this.fields[i][0]);
    return out;
  };

  Assembly.prototype.withField = function (name, value) {
    return Assembly.fromPairs(this.fields.concat([[name, value]]));
  };

  Assembly.prototype.length = function () {
    return this.fields.length;
  };

  function StationValue(module, station) {
    this.type = "station";
    this.module = module;
    this.station = station;
  }

  function BuiltinValue(name) {
    this.type = "builtin";
    this.name = name;
  }

  function kindOf(value) {
    if (value === NOTHING || value === null || value === undefined) return "nothing";
    if (typeof value === "boolean") return "flag";
    if (typeof value === "number") return "number";
    if (typeof value === "string") return "text";
    if (Array.isArray(value)) return "list";
    if (value instanceof Assembly) return "assembly";
    if (value instanceof StationValue) return "station";
    if (value instanceof BuiltinValue) return "builtin";
    return "unknown";
  }

  function truthy(value) {
    if (value === NOTHING || value === null || value === undefined) return false;
    if (typeof value === "boolean") return value;
    if (typeof value === "number") return value !== 0;
    if (typeof value === "string") return value.length > 0;
    if (Array.isArray(value)) return value.length > 0;
    if (value instanceof Assembly) return value.fields.length > 0;
    return true;
  }

  function numberText(value) {
    if (typeof value !== "number") return String(value);
    if (Number.isNaN(value)) return "nan";
    if (value === Infinity) return "inf";
    if (value === -Infinity) return "-inf";
    if (Number.isInteger(value) && Math.abs(value) < 1e16) return String(value);
    return String(value);
  }

  function quoteText(text) {
    var out = ['"'];
    for (var i = 0; i < text.length; i += 1) {
      var ch = text.charAt(i);
      if (ch === '"') out.push('\\"');
      else if (ch === "\\") out.push("\\\\");
      else if (ch === "\n") out.push("\\n");
      else if (ch === "\t") out.push("\\t");
      else if (ch === "\r") out.push("\\r");
      else out.push(ch);
    }
    out.push('"');
    return out.join("");
  }

  function display(value, nested) {
    if (value === NOTHING || value === null || value === undefined) return "nothing";
    if (typeof value === "boolean") return value ? "on" : "off";
    if (typeof value === "number") return numberText(value);
    if (typeof value === "string") return nested ? quoteText(value) : value;
    if (Array.isArray(value)) {
      var parts = [];
      for (var i = 0; i < value.length; i += 1) parts.push(display(value[i], true));
      return "[" + parts.join(", ") + "]";
    }
    if (value instanceof Assembly) {
      var fields = [];
      for (var j = 0; j < value.fields.length; j += 1) {
        fields.push(value.fields[j][0] + ": " + display(value.fields[j][1], true));
      }
      return "assembly{" + fields.join(", ") + "}";
    }
    if (value instanceof StationValue) return "station:" + value.station.name;
    if (value instanceof BuiltinValue) return "builtin:" + value.name;
    return String(value);
  }

  function equal(a, b) {
    var ka = kindOf(a);
    var kb = kindOf(b);
    if (ka !== kb) {
      if (ka === "number" && kb === "number") return a === b;
      return false;
    }
    if (ka === "nothing") return true;
    if (ka === "flag" || ka === "number" || ka === "text") return a === b;
    if (ka === "list") {
      if (a.length !== b.length) return false;
      for (var i = 0; i < a.length; i += 1) if (!equal(a[i], b[i])) return false;
      return true;
    }
    if (ka === "assembly") {
      if (a.fields.length !== b.fields.length) return false;
      for (var j = 0; j < a.fields.length; j += 1) {
        if (a.fields[j][0] !== b.fields[j][0]) return false;
        if (!equal(a.fields[j][1], b.fields[j][1])) return false;
      }
      return true;
    }
    if (ka === "station") return a === b;
    if (ka === "builtin") return a.name === b.name;
    return false;
  }

  function isNumber(value) {
    return typeof value === "number";
  }

  function binary(op, a, b) {
    if (op === "PLUS") {
      if (typeof a === "boolean" || typeof b === "boolean") throw new MiValueError("cannot add flags");
      if (isNumber(a) && isNumber(b)) return a + b;
      if (typeof a === "string" && typeof b === "string") return a + b;
      if (Array.isArray(a) && Array.isArray(b)) return a.concat(b);
      if (a instanceof Assembly && b instanceof Assembly) {
        return Assembly.fromPairs(a.fields.concat(b.fields));
      }
      throw new MiValueError("cannot add " + kindOf(a) + " and " + kindOf(b));
    }
    if (op === "MINUS") {
      if (isNumber(a) && isNumber(b)) return a - b;
      throw new MiValueError("cannot subtract " + kindOf(b) + " from " + kindOf(a));
    }
    if (op === "TIMES") {
      if (typeof a === "boolean" || typeof b === "boolean") throw new MiValueError("cannot multiply flags");
      if (isNumber(a) && isNumber(b)) return a * b;
      if (typeof a === "string" && typeof b === "number" && Number.isInteger(b)) return a.repeat(Math.max(0, b));
      if (Array.isArray(a) && typeof b === "number" && Number.isInteger(b)) {
        var out = [];
        for (var i = 0; i < Math.max(0, b); i += 1) out = out.concat(a);
        return out;
      }
      if (typeof a === "number" && Number.isInteger(a) && typeof b === "string") return b.repeat(Math.max(0, a));
      if (typeof a === "number" && Number.isInteger(a) && Array.isArray(b)) {
        var out2 = [];
        for (var k = 0; k < Math.max(0, a); k += 1) out2 = out2.concat(b);
        return out2;
      }
      throw new MiValueError("cannot multiply " + kindOf(a) + " by " + kindOf(b));
    }
    if (op === "DIV" || op === "IDIV" || op === "MOD") {
      if (!isNumber(a) || !isNumber(b)) {
        throw new MiValueError("cannot divide " + kindOf(a) + " by " + kindOf(b));
      }
      if (b === 0) throw new MiValueError("division by zero", "E0301");
      if (op === "DIV") return a / b;
      if (op === "IDIV") return Math.floor(a / b);
      return a - Math.floor(a / b) * b;
    }
    if (op === "POW") {
      if (!isNumber(a) || !isNumber(b)) throw new MiValueError("cannot raise " + kindOf(a) + " to a power");
      var result = Math.pow(a, b);
      if (Number.isNaN(result)) throw new MiValueError("power failed: math domain error");
      return result;
    }
    if (op === "EQ") return equal(a, b);
    if (op === "NE") return !equal(a, b);
    if (op === "LT" || op === "LE" || op === "GT" || op === "GE") {
      var comparable = (isNumber(a) && isNumber(b)) || (typeof a === "string" && typeof b === "string");
      if (!comparable) throw new MiValueError("cannot compare " + kindOf(a) + " with " + kindOf(b));
      if (op === "LT") return a < b;
      if (op === "LE") return a <= b;
      if (op === "GT") return a > b;
      return a >= b;
    }
    if (op === "AND") return truthy(a) ? b : a;
    if (op === "OR") return truthy(a) ? a : b;
    throw new MiValueError("unknown operator " + op);
  }

  function unary(op, value) {
    if (op === "NOT") return !truthy(value);
    if (op === "MINUS") {
      if (isNumber(value)) return -value;
      throw new MiValueError("cannot negate " + kindOf(value));
    }
    throw new MiValueError("unknown unary operator " + op);
  }

  function asInt(value, what) {
    what = what || "value";
    if (typeof value === "boolean") throw new MiValueError(what + " must be a number, not a flag");
    if (isNumber(value)) {
      if (Number.isInteger(value)) return value;
      throw new MiValueError(what + " must be a whole number");
    }
    throw new MiValueError(what + " must be a number, not " + kindOf(value));
  }

  function asText(value, what) {
    what = what || "value";
    if (typeof value === "string") return value;
    throw new MiValueError(what + " must be text, not " + kindOf(value));
  }

  function asList(value, what) {
    what = what || "value";
    if (Array.isArray(value)) return value;
    throw new MiValueError(what + " must be a list, not " + kindOf(value));
  }

  // =============================================================== modules
  function MiModule(path, name, program) {
    this.path = path;
    this.name = name;
    this.program = program;
    this.stations = {};
    this.imports = {};
    this.flat = [];
  }

  MiModule.prototype.findStation = function (name) {
    if (Object.prototype.hasOwnProperty.call(this.stations, name)) return this.stations[name];
    for (var i = 0; i < this.flat.length; i += 1) {
      if (Object.prototype.hasOwnProperty.call(this.flat[i].stations, name)) return this.flat[i].stations[name];
    }
    return null;
  };

  function ModuleError(diagnostics) {
    this.name = "ModuleError";
    this.diagnostics = diagnostics || [];
    this.message = this.diagnostics.length ? this.diagnostics[0].message : "module error";
  }
  ModuleError.prototype = Object.create(Error.prototype);
  ModuleError.prototype.constructor = ModuleError;

  var VIRTUAL_ORIGINS = { "<source>": 1, "<memory>": 1, "<repl>": 1, "<stdin>": 1, "<string>": 1, "<conformance>": 1 };

  function normalizePath(path) {
    var absolute = path.charAt(0) === "/";
    var parts = path.split("/");
    var out = [];
    for (var i = 0; i < parts.length; i += 1) {
      var part = parts[i];
      if (part === "" || part === ".") continue;
      if (part === "..") {
        // Virtual files are rooted at ""; like Python's .resolve() + relative_to,
        // walking above the root is clamped to the root.
        if (out.length) out.pop();
        continue;
      }
      out.push(part);
    }
    return (absolute ? "/" : "") + out.join("/");
  }

  function dirname(path) {
    var idx = path.lastIndexOf("/");
    return idx < 0 ? "" : path.slice(0, idx);
  }

  function hasMiSuffix(path) {
    return /\.mi$/i.test(path);
  }

  function dummySpan() {
    var pos = { line: 1, col: 1, offset: 0 };
    return { start: pos, end: pos };
  }

  function ModuleLoader(files) {
    this.files = {};
    this.cache = {};
    this.loading = [];
    files = files || {};
    for (var key in files) {
      if (!Object.prototype.hasOwnProperty.call(files, key)) continue;
      this.files[normalizePath(key)] = files[key];
    }
  }

  ModuleLoader.prototype.resolve = function (path, origin) {
    var resolved = path;
    if (!(origin && VIRTUAL_ORIGINS[origin])) {
      var base = origin ? dirname(origin) : "";
      resolved = base ? base + "/" + path : path;
    }
    resolved = normalizePath(resolved);
    if (!hasMiSuffix(resolved)) resolved += ".mi";
    return resolved;
  };

  ModuleLoader.prototype.load = function (path, origin) {
    var resolved = this.resolve(path, origin);
    if (Object.prototype.hasOwnProperty.call(this.cache, resolved)) return this.cache[resolved];
    if (this.loading.indexOf(resolved) >= 0) {
      throw new ModuleError([
        diag("error", "E0307", "module cycle detected while loading `" + path + "`", dummySpan(), "break the `use` cycle between modules")
      ]);
    }
    if (!Object.prototype.hasOwnProperty.call(this.files, resolved)) {
      throw new ModuleError([
        diag("error", "E0307", "cannot find module `" + path + "`", dummySpan(), "this runtime only sees the virtual files given to it")
      ]);
    }
    var text = this.files[resolved];
    this.loading.push(resolved);
    try {
      var lexed = MI.lex(text, resolved);
      var parsed = MI.parse(lexed.tokens);
      var diagnostics = lexed.diagnostics.concat(parsed.diagnostics);
      var self = this;
      diagnostics = diagnostics.concat(
        MI.check(parsed.program, {
          filename: resolved,
          loader: function (p, o) {
            return self.stationParams(p, o);
          }
        })
      );
      diagnostics.sort(function (a, b) {
        var ra = a.severity === "error" ? 0 : 1;
        var rb = b.severity === "error" ? 0 : 1;
        if (ra !== rb) return ra - rb;
        return a.span.start.offset - b.span.start.offset;
      });
      var hasErrors = diagnostics.some(function (d) {
        return d.severity === "error";
      });
      if (hasErrors) throw new ModuleError(diagnostics);
      var module = new MiModule(resolved, resolved.replace(/^.*\//, "").replace(/\.mi$/i, ""), parsed.program);
      for (var i = 0; i < parsed.program.items.length; i += 1) {
        var item = parsed.program.items[i];
        if (item.type === "Station") module.stations[item.name] = item;
      }
      for (var j = 0; j < parsed.program.items.length; j += 1) {
        var use = parsed.program.items[j];
        if (use.type !== "Use") continue;
        var child = this.load(use.path, resolved);
        if (use.alias) module.imports[use.alias] = child;
        else module.flat.push(child);
      }
      this.cache[resolved] = module;
      return module;
    } finally {
      this.loading.pop();
    }
  };

  ModuleLoader.prototype.stationParams = function (path, origin) {
    var module = this.load(path, origin);
    var out = {};
    for (var name in module.stations) {
      if (Object.prototype.hasOwnProperty.call(module.stations, name)) out[name] = module.stations[name].params;
    }
    return out;
  };

  // ================================================================ runtime
  var HaltSignal = { name: "halt" };
  var SkipSignal = { name: "skip" };
  function ReturnSignal(value) {
    this.name = "return";
    this.value = value;
  }
  var NO_RETURN = {};

  function Runtime(opts) {
    opts = opts || {};
    this.loader = opts.loader || new ModuleLoader(opts.files || {});
    this.hosts = opts.hosts || {};
    this.hostName = opts.hostName || "browser";
    this.argv = opts.argv || [];
    this.maxSteps = opts.maxSteps || 5000000;
    this.maxDepth = opts.maxDepth || 400;
    this.onOutput = typeof opts.onOutput === "function" ? opts.onOutput : null;
    this.onError = typeof opts.onError === "function" ? opts.onError : null;
    this.uiHost =
      opts.uiHost ||
      (typeof MI.createUIHost === "function" ? MI.createUIHost({}) : null);
    this.output = [];
    this.steps = 0;
    this.depth = 0;
    this.impls = {};
    var base = MI.builtinImpls || {};
    for (var key in base) if (Object.prototype.hasOwnProperty.call(base, key)) this.impls[key] = base[key];
    var hostImpls = MI.hostImpls || {};
    for (var key2 in hostImpls) if (Object.prototype.hasOwnProperty.call(hostImpls, key2)) this.impls[key2] = hostImpls[key2];
    for (var key3 in this.hosts) if (Object.prototype.hasOwnProperty.call(this.hosts, key3)) this.impls[key3] = this.hosts[key3];
  }

  Runtime.prototype.emitLine = function (text) {
    this.output.push(text);
    if (this.onOutput) this.onOutput(text);
  };

  Runtime.prototype.tick = function (span) {
    this.steps += 1;
    if (this.steps > this.maxSteps) {
      this.fail("E0305", "step budget of " + this.maxSteps + " exhausted", span, "raise it with --max-steps, or make the cycle terminate");
    }
  };

  Runtime.prototype.fail = function (code, message, span, hint) {
    var err = new Error(message);
    err.miError = true;
    err.code = code;
    err.message = message;
    err.span = span || dummySpan();
    err.hint = hint || null;
    throw err;
  };

  Runtime.prototype.run = function (program, filename, module) {
    module = module || new MiModule(filename || "<source>", "main", program);
    var frame = { module: module, station: null, params: {}, wires: {}, variables: {}, ports: {} };
    try {
      this.register(module, program, filename || "<source>");
      this.execItems(program.items, frame);
      return { output: this.output.slice(), error: null };
    } catch (err) {
      if (err && err.miError) return { output: this.output.slice(), error: err };
      if (err instanceof ModuleError) {
        var diagnostic = err.diagnostics[0] || diag("error", "E0307", "module error", dummySpan());
        return {
          output: this.output.slice(),
          error: { miError: true, code: diagnostic.code, message: diagnostic.message, span: diagnostic.span, hint: diagnostic.hint }
        };
      }
      if (err === HaltSignal || err === SkipSignal || err instanceof ReturnSignal) {
        return { output: this.output.slice(), error: null };
      }
      throw err;
    }
  };

  Runtime.prototype.register = function (module, program, origin) {
    var i;
    for (i = 0; i < program.items.length; i += 1) {
      if (program.items[i].type === "Station") module.stations[program.items[i].name] = program.items[i];
    }
    for (i = 0; i < program.items.length; i += 1) {
      var item = program.items[i];
      if (item.type !== "Use") continue;
      var child = this.loader.load(item.path, origin);
      if (item.alias) module.imports[item.alias] = child;
      else module.flat.push(child);
    }
  };

  Runtime.prototype.execItems = function (items, frame) {
    for (var i = 0; i < items.length; i += 1) this.execFlow(items[i], frame);
  };

  Runtime.prototype.execBlock = function (block, frame) {
    for (var i = 0; i < block.flows.length; i += 1) this.execFlow(block.flows[i], frame);
  };

  Runtime.prototype.execFlow = function (flow, frame) {
    this.tick(flow.span);
    var k;
    if (flow.type === "WireDecl") {
      frame.wires[flow.name] = flow.init !== null ? this.eval(flow.init, frame) : NOTHING;
      return;
    }
    if (flow.type === "Emit") {
      if (Object.prototype.hasOwnProperty.call(frame.params, flow.target)) {
        this.fail("E0202", "`" + flow.target + "` is a read-only port", flow.span);
      }
      if (!Object.prototype.hasOwnProperty.call(frame.wires, flow.target)) {
        this.fail("E0200", "wire `" + flow.target + "` is used before it is declared", flow.span);
      }
      frame.wires[flow.target] = this.eval(flow.value, frame);
      return;
    }
    if (flow.type === "When") {
      this.tick(flow.span);
      if (truthy(this.eval(flow.cond, frame))) {
        this.execBlock(flow.body, frame);
      } else if (flow.otherwise) {
        if (flow.otherwise.type === "When") this.execFlow(flow.otherwise, frame);
        else this.execBlock(flow.otherwise, frame);
      }
      return;
    }
    if (flow.type === "Cycle") {
      this.execCycle(flow, frame);
      return;
    }
    if (flow.type === "For") {
      var forItems = asList(this.eval(flow.items, frame), "`for`'s list");
      for (k = 0; k < forItems.length; k += 1) {
        frame.variables[flow.var] = forItems[k];
        this.tick(flow.span);
        try {
          this.execBlock(flow.body, frame);
        } catch (err) {
          if (err === SkipSignal) continue;
          if (err === HaltSignal) break;
          throw err;
        }
      }
      return;
    }
    if (flow.type === "Out") {
      if (frame.station === null) this.fail("E0206", "`out` is only allowed inside a station", flow.span);
      frame.ports[flow.name] = this.eval(flow.value, frame);
      return;
    }
    if (flow.type === "Return") {
      var value = flow.value !== null ? this.eval(flow.value, frame) : NO_RETURN;
      throw new ReturnSignal(value);
    }
    if (flow.type === "Halt") throw HaltSignal;
    if (flow.type === "Skip") throw SkipSignal;
    if (flow.type === "Flow") {
      this.eval(flow.expr, frame);
      return;
    }
  };

  Runtime.prototype.execCycle = function (cycle, frame) {
    var kind = cycle.kind;
    var self = this;
    var i;
    if (kind === "count") {
      var raw = cycle.head !== null ? this.eval(cycle.head, frame) : 0;
      var count;
      try {
        count = asInt(raw, "the cycle count");
      } catch (err) {
        if (err instanceof MiValueError) this.fail(err.code, err.message, cycle.span);
        throw err;
      }
      if (count < 0) this.fail("E0300", "the cycle count cannot be negative", cycle.span);
      for (i = 0; i < count; i += 1) {
        if (cycle.var) frame.variables[cycle.var] = i;
        this.tick(cycle.span);
        try {
          this.execBlock(cycle.body, frame);
        } catch (err) {
          if (err === SkipSignal) continue;
          if (err === HaltSignal) break;
          throw err;
        }
      }
      return;
    }
    if (kind === "while" || kind === "until") {
      var index = 0;
      for (;;) {
        var condition = truthy(this.eval(cycle.head, frame));
        if (kind === "while" && !condition) break;
        if (kind === "until" && condition) break;
        if (cycle.var) frame.variables[cycle.var] = index;
        this.tick(cycle.span);
        try {
          this.execBlock(cycle.body, frame);
        } catch (err) {
          if (err === SkipSignal) {
            index += 1;
            continue;
          }
          if (err === HaltSignal) break;
          throw err;
        }
        index += 1;
      }
      return;
    }
    if (kind === "over") {
      var values = asList(this.eval(cycle.head, frame), "`cycle over`'s list");
      for (i = 0; i < values.length; i += 1) {
        if (cycle.var) frame.variables[cycle.var] = values[i];
        this.tick(cycle.span);
        try {
          this.execBlock(cycle.body, frame);
        } catch (err) {
          if (err === SkipSignal) continue;
          if (err === HaltSignal) break;
          throw err;
        }
      }
      return;
    }
    this.fail("E0300", "unknown cycle kind `" + kind + "`", cycle.span);
  };

  // ------------------------------------------------------------ expressions
  Runtime.prototype.eval = function (expr, frame) {
    this.tick(expr.span);
    var k;
    if (expr.type === "NumberLit") return expr.value;
    if (expr.type === "TextLit") return expr.value;
    if (expr.type === "FlagLit") return expr.value;
    if (expr.type === "NilLit") return NOTHING;
    if (expr.type === "ListLit") {
      var items = [];
      for (k = 0; k < expr.items.length; k += 1) items.push(this.eval(expr.items[k], frame));
      return items;
    }
    if (expr.type === "AssemblyLit") {
      var pairs = [];
      for (k = 0; k < expr.fields.length; k += 1) {
        pairs.push([expr.fields[k].name, this.eval(expr.fields[k].value, frame)]);
      }
      return Assembly.fromPairs(pairs);
    }
    if (expr.type === "Name") return this.evalName(expr, frame);
    if (expr.type === "Read") return this.evalRead(expr, frame);
    if (expr.type === "Field") {
      var base = this.eval(expr.base, frame);
      if (base instanceof Assembly) {
        if (base.has(expr.name)) return base.get(expr.name);
        this.fail(
          "E0302",
          "this assembly has no field `" + expr.name + "`",
          expr.span,
          "fields: " + (base.keys().join(", ") || "none")
        );
      }
      this.fail("E0300", "cannot read field `" + expr.name + "` of " + kindOf(base), expr.span);
    }
    if (expr.type === "Unary") {
      try {
        return unary(expr.op, this.eval(expr.operand, frame));
      } catch (err) {
        if (err instanceof MiValueError) this.fail(err.code, err.message, expr.span);
        throw err;
      }
    }
    if (expr.type === "Binary") {
      var left = this.eval(expr.left, frame);
      if (expr.op === "AND") return truthy(left) ? this.eval(expr.right, frame) : left;
      if (expr.op === "OR") return truthy(left) ? left : this.eval(expr.right, frame);
      var right = this.eval(expr.right, frame);
      try {
        return binary(expr.op, left, right);
      } catch (err2) {
        if (err2 instanceof MiValueError) this.fail(err2.code, err2.message, expr.span);
        throw err2;
      }
    }
    if (expr.type === "Call") return this.evalCall(expr, frame);
    this.fail("E0300", "cannot evaluate this expression", expr.span);
  };

  Runtime.prototype.evalName = function (node, frame) {
    var name = node.name;
    if (node.glyph) {
      var builtin = MI.glyphTable().builtin(name);
      if (!builtin) this.fail("E0203", "unknown builtin `" + name + "`", node.span);
      return new BuiltinValue(builtin.canonical);
    }
    if (Object.prototype.hasOwnProperty.call(frame.params, name)) return frame.params[name];
    if (Object.prototype.hasOwnProperty.call(frame.wires, name)) return frame.wires[name];
    if (Object.prototype.hasOwnProperty.call(frame.variables, name)) return frame.variables[name];
    var station = frame.module.findStation(name);
    if (station !== null) {
      var module = frame.module;
      if (!Object.prototype.hasOwnProperty.call(module.stations, name)) {
        for (var i = 0; i < module.flat.length; i += 1) {
          if (Object.prototype.hasOwnProperty.call(module.flat[i].stations, name)) {
            module = module.flat[i];
            break;
          }
        }
      }
      return new StationValue(module, station);
    }
    var builtin2 = MI.glyphTable().builtin(name);
    if (builtin2 !== null) return new BuiltinValue(builtin2.canonical);
    this.fail("E0203", "unknown name `" + name + "`", node.span);
  };

  Runtime.prototype.evalRead = function (node, frame) {
    if (Object.prototype.hasOwnProperty.call(frame.params, node.name)) return frame.params[node.name];
    if (Object.prototype.hasOwnProperty.call(frame.wires, node.name)) return frame.wires[node.name];
    if (Object.prototype.hasOwnProperty.call(frame.variables, node.name)) return frame.variables[node.name];
    this.fail(
      "E0200",
      "`draw` needs a declared wire, but `" + node.name + "` is not one",
      node.span,
      "declare it with `wire`"
    );
  };

  // ------------------------------------------------------------------ calls
  Runtime.prototype.evalCall = function (call, frame) {
    var pairs = [];
    var i;
    for (i = 0; i < call.args.length; i += 1) pairs.push([call.args[i].label, this.eval(call.args[i].value, frame)]);
    var name = call.name;
    if (name === null) {
      var callee = this.eval(call.callee, frame);
      return this.callValue(callee, pairs, call.span);
    }
    if (call.glyph) {
      var builtin = MI.glyphTable().builtin(name);
      if (!builtin) this.fail("E0203", "unknown builtin `" + name + "`", call.span);
      return this.callBuiltin(builtin.canonical, pairs, call.span);
    }
    if (name.indexOf(".") >= 0) {
      var dot = name.indexOf(".");
      var prefix = name.slice(0, dot);
      var rest = name.slice(dot + 1);
      var imported = frame.module.imports[prefix];
      if (imported && Object.prototype.hasOwnProperty.call(imported.stations, rest)) {
        return this.callStation(imported, imported.stations[rest], pairs, call.span);
      }
      var builtin3 = MI.glyphTable().builtin(name);
      if (builtin3 !== null) return this.callBuiltin(builtin3.canonical, pairs, call.span);
      this.fail("E0203", "unknown name `" + name + "`", call.span, "import the module with `use \u2026 as` first");
    }
    if (Object.prototype.hasOwnProperty.call(frame.params, name)) return this.callValue(frame.params[name], pairs, call.span);
    if (Object.prototype.hasOwnProperty.call(frame.wires, name)) return this.callValue(frame.wires[name], pairs, call.span);
    if (Object.prototype.hasOwnProperty.call(frame.variables, name)) return this.callValue(frame.variables[name], pairs, call.span);
    var station = frame.module.findStation(name);
    if (station !== null) {
      var module = frame.module;
      if (!Object.prototype.hasOwnProperty.call(module.stations, name)) {
        for (i = 0; i < module.flat.length; i += 1) {
          if (Object.prototype.hasOwnProperty.call(module.flat[i].stations, name)) {
            module = module.flat[i];
            break;
          }
        }
      }
      return this.callStation(module, station, pairs, call.span);
    }
    var builtin4 = MI.glyphTable().builtin(name);
    if (builtin4 !== null) return this.callBuiltin(builtin4.canonical, pairs, call.span);
    this.fail("E0203", "unknown station or builtin `" + name + "`", call.span);
  };

  Runtime.prototype.callValue = function (value, pairs, span) {
    if (value instanceof StationValue) return this.callStation(value.module, value.station, pairs, span);
    if (value instanceof BuiltinValue) return this.callBuiltin(value.name, pairs, span);
    this.fail("E0303", "this " + kindOf(value) + " is not callable", span, "put a station or a builtin here");
  };

  Runtime.prototype.callBuiltin = function (name, pairs, span) {
    var builtin = MI.glyphTable().builtin(name);
    if (!builtin) this.fail("E0203", "unknown builtin `" + name + "`", span);
    var hosts = builtin.hosts || [];
    if (hosts.indexOf(this.hostName) < 0) {
      this.fail(
        "E0308",
        "builtin `" + builtin.canonical + "` is not available on the " + this.hostName + " host",
        span,
        "hosts: " + hosts.join(", ")
      );
    }
    var impl = this.impls[builtin.canonical];
    if (!impl) {
      this.fail("E0308", "builtin `" + builtin.canonical + "` has no implementation on this host", span);
    }
    var bound = this.bindBuiltin(builtin, pairs, span);
    try {
      return impl(this, bound, span);
    } catch (err) {
      if (err instanceof MiValueError) this.fail(err.code, err.message, span);
      throw err;
    }
  };

  Runtime.prototype.callStation = function (module, station, pairs, callSpan) {
    this.depth += 1;
    if (this.depth > this.maxDepth) {
      this.depth -= 1;
      this.fail(
        "E0306",
        "call depth limit of " + this.maxDepth + " reached",
        callSpan,
        "check the recursion in `" + station.name + "`"
      );
    }
    var frame = {
      module: module,
      station: station,
      params: {},
      wires: {},
      variables: {},
      ports: {}
    };
    try {
      var bound = this.bindStation(station, pairs, callSpan);
      var name;
      for (name in bound) if (Object.prototype.hasOwnProperty.call(bound, name)) frame.params[name] = bound[name];
      for (var i = 0; i < station.params.length; i += 1) {
        var param = station.params[i];
        if (Object.prototype.hasOwnProperty.call(frame.params, param.name)) continue;
        if (param.default === null) {
          this.fail("E0205", "missing argument `" + param.name + "`", callSpan);
        }
        frame.params[param.name] = this.eval(param.default, frame);
      }
      try {
        this.execBlock(station.body, frame);
      } catch (err) {
        if (err instanceof ReturnSignal) {
          if (err.value !== NO_RETURN) frame.ports.value = err.value;
        } else if (err !== HaltSignal) {
          throw err;
        }
      }
      return this.stationResult(station, frame.ports);
    } finally {
      this.depth -= 1;
    }
  };

  Runtime.prototype.stationResult = function (station, ports) {
    var names = station.outPorts || [];
    if (!names.length) return NOTHING;
    if (names.length === 1) {
      return Object.prototype.hasOwnProperty.call(ports, names[0]) ? ports[names[0]] : NOTHING;
    }
    var pairs = [];
    for (var i = 0; i < names.length; i += 1) {
      if (Object.prototype.hasOwnProperty.call(ports, names[i])) pairs.push([names[i], ports[names[i]]]);
    }
    return Assembly.fromPairs(pairs);
  };

  Runtime.prototype.bindStation = function (station, pairs, span) {
    var names = [];
    for (var i = 0; i < station.params.length; i += 1) names.push(station.params[i].name);
    var bound = {};
    var nextPositional = 0;
    for (var j = 0; j < pairs.length; j += 1) {
      var label = pairs[j][0];
      var value = pairs[j][1];
      if (label === null) {
        if (nextPositional < names.length) {
          bound[names[nextPositional]] = value;
          nextPositional += 1;
        } else {
          this.fail(
            "E0205",
            "too many arguments for station `" + station.name + "`",
            span,
            "ports: " + (names.join(", ") || "none")
          );
        }
      } else {
        if (names.indexOf(label) < 0) {
          this.fail("E0204", "unknown argument label `" + label + "`", span, "ports: " + (names.join(", ") || "none"));
        }
        if (Object.prototype.hasOwnProperty.call(bound, label)) {
          this.fail("E0204", "argument `" + label + "` is given twice", span);
        }
        bound[label] = value;
      }
    }
    return bound;
  };

  Runtime.prototype.bindBuiltin = function (builtin, pairs, span) {
    var names = [];
    for (var i = 0; i < builtin.params.length; i += 1) names.push(builtin.params[i].name);
    var variadic = null;
    for (var v = 0; v < builtin.params.length; v += 1) {
      if (builtin.params[v].variadic) variadic = builtin.params[v].name;
    }
    var bound = {};
    var variadicValues = [];
    var nextPositional = 0;
    for (var j = 0; j < pairs.length; j += 1) {
      var label = pairs[j][0];
      var value = pairs[j][1];
      if (label === null) {
        if (variadic !== null && nextPositional >= names.length - 1) {
          variadicValues.push(value);
        } else if (nextPositional < names.length) {
          bound[names[nextPositional]] = value;
          nextPositional += 1;
        } else {
          this.fail("E0205", "too many arguments for builtin `" + builtin.canonical + "`", span);
        }
      } else {
        if (names.indexOf(label) < 0) {
          this.fail("E0204", "unknown argument label `" + label + "`", span, "arguments: " + (names.join(", ") || "none"));
        }
        if (label === variadic) {
          variadicValues.push(value);
        } else if (Object.prototype.hasOwnProperty.call(bound, label)) {
          this.fail("E0204", "argument `" + label + "` is given twice", span);
        } else {
          bound[label] = value;
        }
      }
    }
    if (variadic !== null) bound[variadic] = variadicValues;
    for (var p = 0; p < builtin.params.length; p += 1) {
      var param = builtin.params[p];
      if (param.optional || param.variadic) continue;
      if (!Object.prototype.hasOwnProperty.call(bound, param.name)) {
        this.fail("E0205", "missing argument `" + param.name + "`", span, "arguments: " + names.join(", "));
      }
    }
    return bound;
  };

  // ================================================================ public
  function sortDiagnostics(diagnostics) {
    return diagnostics.slice().sort(function (a, b) {
      var ra = a.severity === "error" ? 0 : 1;
      var rb = b.severity === "error" ? 0 : 1;
      if (ra !== rb) return ra - rb;
      return a.span.start.offset - b.span.start.offset;
    });
  }

  MI.compile = function (source, options) {
    options = options || {};
    var filename = options.filename || (options.files ? "main.mi" : "<source>");
    var loader = options.loader || new ModuleLoader(options.files || {});
    var lexed = MI.lex(source, filename);
    var parsed = MI.parse(lexed.tokens);
    var diagnostics = lexed.diagnostics.concat(parsed.diagnostics);
    diagnostics = diagnostics.concat(
      MI.check(parsed.program, {
        filename: filename,
        loader: function (path, origin) {
          return loader.stationParams(path, origin);
        }
      })
    );
    diagnostics = sortDiagnostics(diagnostics);
    var hasErrors = diagnostics.some(function (d) {
      return d.severity === "error";
    });
    return { program: parsed.program, diagnostics: diagnostics, hasErrors: hasErrors, loader: loader };
  };

  MI.run = function (program, options) {
    options = options || {};
    var loader = options.loader || new ModuleLoader(options.files || {});
    var runtime = new Runtime({
      loader: loader,
      files: options.files,
      hosts: options.hosts,
      hostName: options.hostName || "browser",
      argv: options.argv,
      maxSteps: options.maxSteps,
      maxDepth: options.maxDepth,
      onOutput: options.onOutput,
      onError: options.onError,
      uiHost: options.uiHost
    });
    var filename = options.filename || (options.files ? "main.mi" : "<source>");
    var result = runtime.run(program, filename);
    var diagnostics = [];
    if (result.error) {
      diagnostics.push(
        diag("error", result.error.code, result.error.message, result.error.span || dummySpan(), result.error.hint)
      );
    }
    return { output: result.output, diagnostics: diagnostics };
  };

  MI.execute = function (source, options) {
    options = options || {};
    var compiled = MI.compile(source, options);
    var diagnostics = compiled.diagnostics.slice();
    if (compiled.hasErrors) {
      return { output: [], diagnostics: diagnostics };
    }
    var result = MI.run(compiled.program, {
      loader: compiled.loader,
      files: options.files,
      hosts: options.hosts,
      hostName: options.hostName || "browser",
      argv: options.argv,
      maxSteps: options.maxSteps,
      maxDepth: options.maxDepth,
      onOutput: options.onOutput,
      onError: options.onError,
      uiHost: options.uiHost,
      filename: options.filename || (options.files ? "main.mi" : "<source>")
    });
    diagnostics = diagnostics.concat(result.diagnostics);
    return { output: result.output, diagnostics: diagnostics };
  };

  MI._internals.values = {
    NOTHING: NOTHING,
    Assembly: Assembly,
    StationValue: StationValue,
    BuiltinValue: BuiltinValue,
    kindOf: kindOf,
    truthy: truthy,
    display: display,
    numberText: numberText,
    equal: equal,
    binary: binary,
    unary: unary,
    asInt: asInt,
    asText: asText,
    asList: asList,
    MiValueError: MiValueError,
    ModuleLoader: ModuleLoader,
    MiModule: MiModule,
    ModuleError: ModuleError,
    diag: diag,
    dummySpan: dummySpan
  };
})();
