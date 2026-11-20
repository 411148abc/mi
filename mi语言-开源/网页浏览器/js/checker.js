/* MI browser runtime -- static checker.
 *
 * Classic script: MI.check(program, {filename, loader}) -> Diagnostic[].
 * Port of 米/checker.py: undeclared wires, reads of ports, unknown
 * stations/builtins, argument-label and arity mistakes, misplaced flows, and
 * the W0100..W0103 warnings.  It never executes code.
 */
(function () {
  "use strict";

  window.MI = window.MI || {};
  var MI = window.MI;

  var internals = MI._internals || (MI._internals = {});
  var walkFlows = internals.walkFlows;
  var pyRepr = internals.pyRepr;

  function diag(severity, code, message, span, hint) {
    return { severity: severity, code: code, message: message, span: span, hint: hint || null };
  }

  function lcsLength(a, b) {
    var al = a.length;
    var bl = b.length;
    if (!al || !bl) return 0;
    var prev = new Array(bl + 1);
    var cur = new Array(bl + 1);
    var i;
    var j;
    for (j = 0; j <= bl; j += 1) prev[j] = 0;
    for (i = 1; i <= al; i += 1) {
      cur[0] = 0;
      for (j = 1; j <= bl; j += 1) {
        cur[j] = a.charAt(i - 1) === b.charAt(j - 1) ? prev[j - 1] + 1 : Math.max(prev[j], cur[j - 1]);
      }
      var tmp = prev;
      prev = cur;
      cur = tmp;
    }
    return prev[bl];
  }

  // difflib.SequenceMatcher.ratio(): 2 * matching-characters / total length.
  function ratio(a, b) {
    var total = a.length + b.length;
    if (!total) return 1;
    return (2 * lcsLength(a, b)) / total;
  }

  function suggest(word, candidates, n, cutoff) {
    if (!word) return [];
    n = n || 3;
    if (cutoff === undefined) cutoff = 0.6;
    var low = word.toLowerCase();
    var scored = [];
    var i;
    for (i = 0; i < candidates.length; i += 1) {
      var cand = String(candidates[i]);
      var score = ratio(low, cand.toLowerCase());
      if (score >= cutoff) scored.push([score, cand]);
    }
    if (!scored.length) {
      var relaxed = Math.max(0.4, cutoff - 0.1);
      for (i = 0; i < candidates.length; i += 1) {
        var cand2 = String(candidates[i]);
        var score2 = ratio(low, cand2.toLowerCase());
        if (score2 >= relaxed) scored.push([score2, cand2]);
      }
    }
    // difflib sorts (ratio, candidate) tuples descending.
    scored.sort(function (x, y) {
      if (y[0] !== x[0]) return y[0] - x[0];
      return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
    });
    return scored.slice(0, n).map(function (pair) {
      return pair[1];
    });
  }

  function makeSet() {
    return Object.create(null);
  }

  function setAdd(set, value) {
    set[value] = true;
  }

  function inSet(set, value) {
    return !!set[value];
  }

  function setValues(set) {
    return Object.keys(set);
  }

  function cloneSet(set) {
    var out = makeSet();
    for (var k in set) if (Object.prototype.hasOwnProperty.call(set, k)) out[k] = true;
    return out;
  }

  function Scope(fields) {
    fields = fields || {};
    this.wires = fields.wires || makeSet();
    this.params = fields.params || makeSet();
    this.variables = fields.variables || makeSet();
    this.aliases = fields.aliases || makeSet();
    this.allowUnknownStations = !!fields.allowUnknownStations;
  }

  Scope.prototype.clone = function () {
    return new Scope({
      wires: cloneSet(this.wires),
      params: cloneSet(this.params),
      variables: cloneSet(this.variables),
      aliases: cloneSet(this.aliases),
      allowUnknownStations: this.allowUnknownStations
    });
  };

  Scope.prototype.hasValue = function (name) {
    return inSet(this.wires, name) || inSet(this.params, name) || inSet(this.variables, name);
  };

  function Checker(program) {
    var opts = arguments.length > 1 && arguments[1] !== undefined ? arguments[1] : {};
    this.program = program;
    this.filename = opts.filename || "<source>";
    this.loader = opts.loader || null;
    this.table = MI.glyphTable();
    this.diagnostics = [];
    this.stations = {};
    this.aliases = makeSet();
    this.flatImport = false;
    this.usage = { wires: [], reads: makeSet() };
  }

  Checker.prototype.error = function (code, message, span, hint) {
    this.diagnostics.push(diag("error", code, message, span, hint || null));
  };

  Checker.prototype.warning = function (code, message, span, hint) {
    this.diagnostics.push(diag("warning", code, message, span, hint || null));
  };

  Checker.prototype.check = function () {
    this.collectTopLevel();
    var root = new Scope({ aliases: cloneSet(this.aliases), allowUnknownStations: this.flatImport });
    var topItems = [];
    for (var i = 0; i < this.program.items.length; i += 1) {
      var item = this.program.items[i];
      if (item.type !== "Station" && item.type !== "Use") topItems.push(item);
    }
    this.checkBlockItems(topItems, root, false, 0);
    for (var w = 0; w < this.usage.wires.length; w += 1) {
      var pair = this.usage.wires[w];
      if (!inSet(this.usage.reads, pair[0])) {
        this.warning(
          "W0100",
          "wire `" + pair[0] + "` is declared but never read",
          pair[1],
          "remove it, or read it with `draw`"
        );
      }
    }
    return this.sortedDiagnostics();
  };

  Checker.prototype.sortedDiagnostics = function () {
    var rank = { error: 0, warning: 1 };
    return this.diagnostics.slice().sort(function (a, b) {
      var ra = rank[a.severity] === undefined ? 2 : rank[a.severity];
      var rb = rank[b.severity] === undefined ? 2 : rank[b.severity];
      if (ra !== rb) return ra - rb;
      return a.span.start.offset - b.span.start.offset;
    });
  };

  // ------------------------------------------------------------ collecting
  Checker.prototype.collectTopLevel = function () {
    var i;
    for (i = 0; i < this.program.items.length; i += 1) {
      var item = this.program.items[i];
      if (item.type === "Station") {
        if (Object.prototype.hasOwnProperty.call(this.stations, item.name)) {
          this.error("E0201", "station `" + item.name + "` is declared twice", item.span);
          continue;
        }
        this.stations[item.name] = item;
      }
    }
    for (i = 0; i < this.program.items.length; i += 1) {
      if (this.program.items[i].type === "Use") this.handleUse(this.program.items[i]);
    }
    for (i = 0; i < this.program.items.length; i += 1) {
      if (this.program.items[i].type === "Station") this.checkStation(this.program.items[i]);
    }
    this.usage = { wires: [], reads: makeSet() };
  };

  Checker.prototype.handleUse = function (item) {
    if (item.alias) setAdd(this.aliases, item.alias);
    else this.flatImport = true;
    if (!this.loader) return;
    var names;
    try {
      names = this.loader(item.path, this.filename);
    } catch (exc) {
      var diagnostics = exc && exc.diagnostics;
      if (diagnostics && diagnostics.length) {
        for (var d = 0; d < diagnostics.length; d += 1) this.diagnostics.push(diagnostics[d]);
      } else {
        this.error("E0307", "cannot load module `" + item.path + "`", item.span);
      }
      return;
    }
    for (var name in names) {
      if (!Object.prototype.hasOwnProperty.call(names, name)) continue;
      if (Object.prototype.hasOwnProperty.call(this.stations, name)) {
        this.error(
          "E0201",
          "station `" + name + "` from `" + item.path + "` collides with a local station",
          item.span,
          "import the module with `as` and call `alias.name(...)`"
        );
      }
    }
  };

  // -------------------------------------------------------------- stations
  Checker.prototype.checkStation = function (station) {
    var savedUsage = this.usage;
    this.usage = { wires: [], reads: makeSet() };
    var scope = new Scope({ aliases: cloneSet(this.aliases), allowUnknownStations: this.flatImport });
    var seenParams = makeSet();
    for (var i = 0; i < station.params.length; i += 1) {
      var param = station.params[i];
      if (inSet(seenParams, param.name)) {
        this.error("E0201", "parameter `" + param.name + "` is declared twice", param.span);
      }
      setAdd(seenParams, param.name);
      if (param.default !== null) this.checkExpr(param.default, scope);
      setAdd(scope.params, param.name);
    }
    for (var f = 0; f < station.body.flows.length; f += 1) {
      this.checkFlow(station.body.flows[f], scope, true, 0);
    }
    for (var w = 0; w < this.usage.wires.length; w += 1) {
      var pair = this.usage.wires[w];
      if (!inSet(this.usage.reads, pair[0])) {
        this.warning(
          "W0100",
          "wire `" + pair[0] + "` is declared but never read",
          pair[1],
          "remove it, or read it with `draw`"
        );
      }
    }
    this.usage = savedUsage;
  };

  // ------------------------------------------------------------------ items
  Checker.prototype.checkBlockItems = function (items, scope, inStation, cycleDepth) {
    var terminated = false;
    for (var i = 0; i < items.length; i += 1) {
      var item = items[i];
      if (terminated) {
        this.warning("W0103", "this flow can never run", item.span || this.program.span);
      }
      this.checkFlow(item, scope, inStation, cycleDepth);
      if (item.type === "Halt" || item.type === "Return") terminated = true;
    }
  };

  Checker.prototype.checkFlow = function (flow, scope, inStation, cycleDepth) {
    var k;
    if (flow.type === "WireDecl") {
      if (flow.init !== null) this.checkExpr(flow.init, scope);
      if (inSet(scope.params, flow.name) || inSet(scope.variables, flow.name)) {
        this.error(
          "E0202",
          "`" + flow.name + "` is a read-only port and cannot be redeclared as a wire",
          flow.span
        );
      } else if (inSet(scope.wires, flow.name)) {
        this.error("E0201", "wire `" + flow.name + "` is declared twice", flow.span);
      } else {
        if (this.builtin(flow.name) !== null) {
          this.warning(
            "W0102",
            "wire `" + flow.name + "` shadows the builtin of the same name",
            flow.span,
            "pick another name, or call the builtin before shadowing"
          );
        }
        setAdd(scope.wires, flow.name);
        this.usage.wires.push([flow.name, flow.span]);
      }
    } else if (flow.type === "Emit") {
      if (inSet(scope.params, flow.target) || inSet(scope.variables, flow.target)) {
        this.error(
          "E0202",
          "`" + flow.target + "` is a read-only port and cannot be written",
          flow.span,
          "declare a local wire with `wire` and emit into that"
        );
      } else if (!inSet(scope.wires, flow.target)) {
        var close = suggest(flow.target, setValues(scope.wires).sort(), 1);
        var hint = close.length ? "did you mean `" + close[0] + "`?" : "declare it with `wire`";
        this.error(
          "E0200",
          "wire `" + flow.target + "` is used before it is declared",
          flow.span,
          hint
        );
      }
      this.checkExpr(flow.value, scope);
    } else if (flow.type === "When") {
      this.checkExpr(flow.cond, scope);
      this.checkBlockItems(flow.body.flows, scope.clone(), inStation, cycleDepth);
      if (flow.otherwise) {
        if (flow.otherwise.type === "When") {
          this.checkFlow(flow.otherwise, scope.clone(), inStation, cycleDepth);
        } else {
          this.checkBlockItems(flow.otherwise.flows, scope.clone(), inStation, cycleDepth);
        }
      }
    } else if (flow.type === "Cycle") {
      if (flow.head !== null) this.checkExpr(flow.head, scope);
      var inner = scope.clone();
      if (flow.var) setAdd(inner.variables, flow.var);
      this.checkBlockItems(flow.body.flows, inner, inStation, cycleDepth + 1);
      if ((flow.kind === "while" || flow.kind === "until") && !hasExit(flow.body)) {
        this.warning(
          "W0101",
          "this cycle has no `halt` and may never finish",
          flow.span,
          "add a `halt`, or iterate with `cycle over`"
        );
      }
    } else if (flow.type === "For") {
      this.checkExpr(flow.items, scope);
      var inner2 = scope.clone();
      setAdd(inner2.variables, flow.var);
      this.checkBlockItems(flow.body.flows, inner2, inStation, cycleDepth + 1);
    } else if (flow.type === "Out") {
      if (!inStation) this.error("E0206", "`out` is only allowed inside a station", flow.span);
      else this.checkExpr(flow.value, scope);
    } else if (flow.type === "Return") {
      if (!inStation) this.error("E0206", "`return` is only allowed inside a station", flow.span);
      if (flow.value !== null) this.checkExpr(flow.value, scope);
    } else if (flow.type === "Halt") {
      // no checks
    } else if (flow.type === "Skip") {
      if (cycleDepth === 0) {
        this.error(
          "E0304",
          "`skip` can only be used inside a cycle",
          flow.span,
          "use `halt` to leave a station"
        );
      }
    } else if (flow.type === "Station") {
      this.error(
        "E0206",
        "stations must be declared at the top level",
        flow.span,
        "move it out of the block"
      );
      this.checkStation(flow);
    } else if (flow.type === "Use") {
      this.error("E0206", "`use` is only allowed at the top level", flow.span);
    } else if (flow.type === "Flow") {
      this.checkExpr(flow.expr, scope);
    }
  };

  // ----------------------------------------------------------- expressions
  Checker.prototype.checkExpr = function (expr, scope) {
    var i;
    if (
      expr.type === "NumberLit" ||
      expr.type === "TextLit" ||
      expr.type === "FlagLit" ||
      expr.type === "NilLit"
    ) {
      return;
    }
    if (expr.type === "ListLit") {
      for (i = 0; i < expr.items.length; i += 1) this.checkExpr(expr.items[i], scope);
      return;
    }
    if (expr.type === "AssemblyLit") {
      for (i = 0; i < expr.fields.length; i += 1) this.checkExpr(expr.fields[i].value, scope);
      return;
    }
    if (expr.type === "Name") {
      this.checkValueReference(expr.name, expr, scope, expr.glyph);
      return;
    }
    if (expr.type === "Read") {
      if (inSet(scope.params, expr.name) || inSet(scope.variables, expr.name)) {
        setAdd(this.usage.reads, expr.name);
        return;
      }
      if (inSet(scope.wires, expr.name)) {
        setAdd(this.usage.reads, expr.name);
        return;
      }
      var close = suggest(expr.name, setValues(scope.wires).sort(), 1);
      var hint = close.length ? "did you mean `" + close[0] + "`?" : "declare it with `wire` first";
      this.error(
        "E0200",
        "`draw` needs a declared wire, but `" + expr.name + "` is not one",
        expr.span,
        hint
      );
      return;
    }
    if (expr.type === "Field") {
      this.checkExpr(expr.base, scope);
      return;
    }
    if (expr.type === "Unary") {
      this.checkExpr(expr.operand, scope);
      return;
    }
    if (expr.type === "Binary") {
      this.checkExpr(expr.left, scope);
      this.checkExpr(expr.right, scope);
      return;
    }
    if (expr.type === "Call") {
      this.checkCall(expr, scope);
      return;
    }
  };

  Checker.prototype.checkValueReference = function (name, node, scope, glyph) {
    if (glyph) {
      if (this.table.builtin(name) === null) {
        this.error("E0203", "unknown builtin glyph `" + name + "`", node.span);
      }
      return;
    }
    if (scope.hasValue(name)) {
      if (inSet(scope.wires, name)) setAdd(this.usage.reads, name);
      return;
    }
    if (name.indexOf(".") >= 0) {
      var prefix = name.split(".")[0];
      if (inSet(scope.aliases, prefix)) return;
      if (this.table.builtin(name) !== null) return;
      this.error(
        "E0203",
        "unknown name `" + name + "`",
        node.span,
        "import the module with `use \u2026 as` first"
      );
      return;
    }
    if (Object.prototype.hasOwnProperty.call(this.stations, name)) return;
    if (this.builtin(name) !== null) return;
    if (scope.allowUnknownStations) return;
    var candidates = setValues(scope.wires)
      .concat(setValues(scope.params))
      .concat(setValues(scope.variables))
      .concat(Object.keys(this.stations))
      .concat(
        this.table.builtins.map(function (b) {
          return b.canonical;
        })
      );
    this.unknownName(name, node.span, candidates, "name");
  };

  Checker.prototype.keywordSuggestion = function (name) {
    var matches = suggest(name, this.table.keywordWords, 1, 0.72);
    if (!matches.length) return null;
    var word = matches[0];
    var tokenName = this.table.isKeywordSpelling(word);
    var keyword = tokenName ? this.table.keywordByName[tokenName] : null;
    var aliases = keyword ? keyword.spellings.join(" | ") : word;
    return [word, aliases];
  };

  Checker.prototype.unknownName = function (name, span, known, what) {
    var close = suggest(name, known, 2, 0.6);
    if (close.length) {
      var hint =
        "maybe you meant " +
        close
          .map(function (c) {
            return "`" + c + "`";
          })
          .join(" or ") +
        "?";
      this.error("E0203", "unknown " + what + " `" + name + "`", span, hint);
      return;
    }
    var keyword = this.keywordSuggestion(name);
    if (keyword !== null) {
      this.error(
        "E0101",
        "unknown keyword `" + name + "`",
        span,
        "maybe you meant `" + keyword[0] + "` (also written: " + keyword[1] + ")"
      );
      return;
    }
    this.error("E0203", "unknown " + what + " `" + name + "`", span);
  };

  Checker.prototype.checkArgValues = function (call, scope) {
    for (var i = 0; i < call.args.length; i += 1) this.checkExpr(call.args[i].value, scope);
  };

  Checker.prototype.checkCall = function (call, scope) {
    var i;
    var builtin = null;
    var name = call.name;
    if (call.glyph && name !== null) {
      builtin = this.table.builtin(name);
      if (builtin === null) {
        this.error("E0203", "unknown builtin `" + name + "`", call.span);
        this.checkArgValues(call, scope);
        return;
      }
      this.checkExprArgs(call, builtin.params, call.span, scope);
      return;
    }
    if (name === null) {
      if (call.callee !== null) this.checkExpr(call.callee, scope);
      this.checkArgValues(call, scope);
      return;
    }
    if (name.indexOf(".") >= 0) {
      var prefix = name.split(".")[0];
      if (inSet(scope.aliases, prefix)) {
        this.checkArgValues(call, scope);
        return;
      }
      builtin = this.table.builtin(name);
      if (builtin === null) {
        this.error(
          "E0203",
          "unknown name `" + name + "`",
          call.span,
          "module aliases must be imported with `use \u2026 as`"
        );
        this.checkArgValues(call, scope);
        return;
      }
      this.checkExprArgs(call, builtin.params, call.span, scope);
      return;
    }
    if (scope.hasValue(name)) {
      this.checkArgValues(call, scope);
      return;
    }
    if (Object.prototype.hasOwnProperty.call(this.stations, name)) {
      this.checkExprArgs(call, this.stations[name].params, call.span, scope);
      return;
    }
    builtin = this.builtin(name);
    if (builtin !== null) {
      this.checkExprArgs(call, builtin.params, call.span, scope);
      return;
    }
    if (scope.allowUnknownStations) {
      this.checkArgValues(call, scope);
      return;
    }
    if (call.keyword_typo) {
      // the parser already reported E0101 with a keyword suggestion
      this.checkArgValues(call, scope);
      return;
    }
    var candidates = Object.keys(this.stations).concat(
      this.table.builtins.map(function (b) {
        return b.canonical;
      })
    );
    candidates = candidates
      .concat(setValues(scope.wires))
      .concat(setValues(scope.params))
      .concat(setValues(scope.variables));
    var close = suggest(name, candidates, 2);
    var hint = null;
    if (close.length) {
      hint =
        "maybe you meant " +
        close
          .map(function (c) {
            return "`" + c + "`";
          })
          .join(" or ") +
        "?";
    }
    this.error("E0203", "unknown station or builtin `" + name + "`", call.span, hint);
    this.checkArgValues(call, scope);
  };

  Checker.prototype.checkExprArgs = function (call, params, span, scope) {
    var names = [];
    var optional = makeSet();
    var variadic = null;
    var i;
    for (i = 0; i < params.length; i += 1) {
      var param = params[i];
      if (param.type === "Param") {
        names.push(param.name);
        if (param.default !== null) setAdd(optional, param.name);
      } else {
        names.push(param.name);
        if (param.optional) setAdd(optional, param.name);
        if (param.variadic) variadic = param.name;
      }
    }
    var positional = [];
    var labelled = makeSet();
    var overflow = false;
    var labelError = false;
    for (i = 0; i < call.args.length; i += 1) {
      var arg = call.args[i];
      this.checkExpr(arg.value, scope);
      if (arg.label === null) {
        if (positional.length >= names.length) {
          if (variadic === null) overflow = true;
        } else {
          positional.push(names[positional.length]);
        }
      } else if (names.indexOf(arg.label) < 0) {
        if (variadic !== null) continue;
        var close = suggest(arg.label, names, 1);
        var hint = close.length
          ? "did you mean `" + close[0] + "`?"
          : "parameters: " + (names.join(", ") || "none");
        this.error("E0204", "unknown argument label `" + arg.label + "`", arg.span, hint);
        labelError = true;
      } else if (inSet(labelled, arg.label)) {
        this.error("E0204", "argument `" + arg.label + "` is given twice", arg.span);
      } else {
        setAdd(labelled, arg.label);
      }
    }
    if (overflow) {
      this.error(
        "E0205",
        "too many arguments",
        span,
        "this station/builtin takes " + names.length + " port(s)"
      );
    }
    if (labelError) return;
    for (i = 0; i < names.length; i += 1) {
      var required = names[i];
      if (inSet(optional, required) || required === variadic) continue;
      if (positional.indexOf(required) < 0 && !inSet(labelled, required)) {
        this.error(
          "E0205",
          "missing argument `" + required + "`",
          span,
          "arguments: " + names.join(", ")
        );
      }
    }
  };

  Checker.prototype.builtin = function (name) {
    return this.table.builtin(name);
  };

  function hasExit(block) {
    var flows = walkFlows(block);
    for (var i = 0; i < flows.length; i += 1) {
      var flow = flows[i];
      if (flow.type === "Halt" || flow.type === "Return") return true;
      if (flow.type === "Cycle") {
        var inner = walkFlows(flow.body);
        for (var j = 0; j < inner.length; j += 1) {
          if (inner[j].type === "Halt") return true;
        }
      }
    }
    return false;
  }

  MI.check = function (program, opts) {
    var checker = new Checker(program, opts || {});
    return checker.check();
  };
  MI._internals.suggest = suggest;
  MI._internals.diag = diag;
})();
