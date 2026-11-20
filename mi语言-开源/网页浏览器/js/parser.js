/* MI browser runtime -- parser.
 *
 * Classic script: MI.parse(tokens) -> {program, diagnostics}.  Port of
 * 米/parser.py: recursive descent with error recovery.  Accepts every surface
 * form in SPEC.md: alternative spellings, optional punctuation, optional
 * parentheses, implicit calls, infix emit, {} and do..end blocks, otherwise
 * when chains.
 */
(function () {
  "use strict";

  window.MI = window.MI || {};
  var MI = window.MI;

  var EXPRESSION_START_KINDS = { IDENT: 1, NUMBER: 1, TEXT: 1, FLAG: 1, NIL: 1, GLYPH: 1 };

  function Recovers(message) {
    this.name = "Recover";
    this.message = message || "";
  }
  Recovers.prototype = Object.create(Error.prototype);
  Recovers.prototype.constructor = Recovers;

  function node(type, span, fields) {
    var out = { type: type, span: span };
    if (fields) {
      for (var key in fields) {
        if (Object.prototype.hasOwnProperty.call(fields, key)) out[key] = fields[key];
      }
    }
    return out;
  }

  function spanOf(nodeOrSpan) {
    return nodeOrSpan && nodeOrSpan.span ? nodeOrSpan.span : nodeOrSpan;
  }

  function spanBetween(first, last) {
    return { start: spanOf(first).start, end: spanOf(last).end };
  }

  function pyRepr(text) {
    if (text === null || text === undefined) return "None";
    var hasSingle = text.indexOf("'") >= 0;
    var hasDouble = text.indexOf('"') >= 0;
    if (hasSingle && !hasDouble) {
      var body = text
        .replace(/\\/g, "\\\\")
        .replace(/"/g, '\\"')
        .replace(/\n/g, "\\n")
        .replace(/\t/g, "\\t")
        .replace(/\r/g, "\\r");
      return '"' + body + '"';
    }
    var body2 = text
      .replace(/\\/g, "\\\\")
      .replace(/'/g, "\\'")
      .replace(/\n/g, "\\n")
      .replace(/\t/g, "\\t")
      .replace(/\r/g, "\\r");
    return "'" + body2 + "'";
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

  // difflib.SequenceMatcher.ratio(): 2 * matching characters / total length.
  function sequenceRatio(a, b) {
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
      var score = sequenceRatio(low, cand.toLowerCase());
      if (score >= cutoff) scored.push([score, cand]);
    }
    if (!scored.length) {
      var relaxed = Math.max(0.4, cutoff - 0.1);
      for (i = 0; i < candidates.length; i += 1) {
        var cand2 = String(candidates[i]);
        var score2 = sequenceRatio(low, cand2.toLowerCase());
        if (score2 >= relaxed) scored.push([score2, cand2]);
      }
    }
    scored.sort(function (x, y) {
      if (y[0] !== x[0]) return y[0] - x[0];
      return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
    });
    return scored.slice(0, n).map(function (pair) {
      return pair[1];
    });
  }

  function diag(severity, code, message, span, hint) {
    return { severity: severity, code: code, message: message, span: span, hint: hint || null };
  }

  function Parser(tokens) {
    this.table = MI.glyphTable();
    this.tokens = this.stripInnerSeps(tokens);
    this.i = 0;
    this.diagnostics = [];
  }

  Parser.prototype.stripInnerSeps = function (tokens) {
    var depth = 0;
    var out = [];
    for (var i = 0; i < tokens.length; i += 1) {
      var tok = tokens[i];
      if (tok.kind === "PUNCT" && "([\u27e8".indexOf(tok.value) >= 0) depth += 1;
      else if (tok.kind === "PUNCT" && ")]\u27e9".indexOf(tok.value) >= 0) depth = Math.max(0, depth - 1);
      if (tok.kind === "SEP" && depth > 0) continue;
      out.push(tok);
    }
    return out;
  };

  Parser.prototype.peek = function (k) {
    var idx = Math.min(this.i + (k || 0), this.tokens.length - 1);
    return this.tokens[idx];
  };

  Parser.prototype.advance = function () {
    var tok = this.peek();
    if (tok.kind !== "EOF") this.i += 1;
    return tok;
  };

  Parser.prototype.atEof = function () {
    return this.peek().kind === "EOF";
  };

  Parser.prototype.atSep = function () {
    return this.peek().kind === "SEP";
  };

  Parser.prototype.skipSeps = function () {
    while (this.peek().kind === "SEP") this.advance();
  };

  Parser.prototype.atPunct = function (value) {
    var tok = this.peek();
    return tok.kind === "PUNCT" && tok.value === value;
  };

  Parser.prototype.atKw = function () {
    var tok = this.peek();
    if (tok.kind !== "KW") return false;
    for (var i = 0; i < arguments.length; i += 1) if (tok.value === arguments[i]) return true;
    return false;
  };

  Parser.prototype.atOp = function (value) {
    var tok = this.peek();
    return tok.kind === "OP" && tok.value === value;
  };

  Parser.prototype.error = function (code, message, span, hint) {
    this.diagnostics.push(diag("error", code, message, span, hint || null));
  };

  Parser.prototype.unexpected = function (expected, tok) {
    tok = tok || this.peek();
    var shown = tok.text ? tok.text : "end of input";
    var hint = null;
    if (tok.text) {
      var close = suggest(tok.text, this.table.suggestionWords, 2);
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
    }
    this.error(
      "E0100",
      "unexpected " + pyRepr(shown) + "; expected " + expected,
      tok.span,
      hint
    );
  };

  Parser.prototype.expectPunct = function (value) {
    if (this.atPunct(value)) return this.advance();
    this.unexpected("`" + value + "`");
    throw new Recovers();
  };

  Parser.prototype.expectKw = function () {
    var tok = this.peek();
    if (tok.kind === "KW") {
      for (var i = 0; i < arguments.length; i += 1) {
        if (tok.value === arguments[i]) return this.advance();
      }
    }
    var names = [];
    for (var k = 0; k < arguments.length; k += 1) names.push("`" + arguments[k].toLowerCase() + "`");
    this.unexpected(names.join(" or "));
    throw new Recovers();
  };

  Parser.prototype.expectIdent = function () {
    var tok = this.peek();
    if (tok.kind === "IDENT") return this.advance();
    if (tok.kind === "KW") {
      this.error(
        "E0100",
        "`" + tok.text + "` is a keyword and cannot be used as a name",
        tok.span,
        "pick a different name"
      );
      var advanced = this.advance();
      return { kind: "IDENT", value: advanced.text, text: advanced.text, span: advanced.span };
    }
    this.unexpected("a name");
    throw new Recovers();
  };

  Parser.prototype.recover = function () {
    while (!this.atEof()) {
      if (this.atSep()) {
        this.advance();
        return;
      }
      if (this.atPunct("}") || this.atKw("END")) return;
      this.advance();
    }
  };

  // ---------------------------------------------------------------- program
  Parser.prototype.parseProgram = function () {
    var start = this.peek().span;
    var items = [];
    this.skipSeps();
    while (!this.atEof()) {
      var before = this.i;
      var item = null;
      try {
        item = this.parseItem();
      } catch (err) {
        if (!(err instanceof Recovers)) throw err;
        if (this.i === before) this.advance();
        this.recover();
        this.skipSeps();
        continue;
      }
      if (item) items.push(item);
      if (this.i === before) this.advance();
      this.skipSeps();
    }
    var end = this.peek().span;
    return node("Program", { start: start.start, end: end.end }, { items: items });
  };

  Parser.prototype.parseItem = function () {
    var tok = this.peek();
    if (tok.kind === "KW") {
      if (tok.value === "WIRE") return this.parseWire();
      if (tok.value === "STATION") return this.parseStation();
      if (tok.value === "USE") return this.parseUse();
    }
    return this.parseFlow();
  };

  // ------------------------------------------------------------------ decls
  Parser.prototype.parseWire = function () {
    var tok = this.expectKw("WIRE");
    var name = this.expectIdent();
    var init = null;
    if (this.atPunct(":") || this.atPunct("=")) {
      this.advance();
      init = this.parseExpr();
    }
    var end = init ? init.span : name.span;
    return node("WireDecl", { start: tok.span.start, end: end.end }, { name: name.value, init: init });
  };

  Parser.prototype.parseUse = function () {
    var tok = this.expectKw("USE");
    if (this.peek().kind !== "TEXT") {
      this.error("E0100", "expected a module path text after `use`", this.peek().span);
      throw new Recovers();
    }
    var path = this.advance();
    var alias = null;
    var end = path.span;
    if (this.atKw("AS")) {
      this.advance();
      var aliasTok = this.expectIdent();
      alias = aliasTok.value;
      end = aliasTok.span;
    }
    return node("Use", { start: tok.span.start, end: end.end }, { path: path.value, alias: alias });
  };

  Parser.prototype.parseStation = function () {
    var tok = this.expectKw("STATION");
    var name = this.expectIdent();
    this.expectPunct("(");
    var params = [];
    if (!this.atPunct(")")) {
      for (;;) {
        var paramTok = this.expectIdent();
        var def = null;
        if (this.atPunct(":") || this.atPunct("=")) {
          this.advance();
          def = this.parseExpr();
        }
        var span = paramTok.span;
        if (def) span = { start: paramTok.span.start, end: def.span.end };
        params.push(node("Param", span, { name: paramTok.value, default: def }));
        if (this.atPunct(",")) {
          this.advance();
          continue;
        }
        if (this.atPunct(")")) break;
        this.error("E0103", "expected `,` or `)` in the parameter list", this.peek().span);
        break;
      }
    }
    this.expectPunct(")");
    var body = this.parseBlock();
    var station = node("Station", { start: tok.span.start, end: body.span.end }, {
      name: name.value,
      params: params,
      body: body
    });
    station.outPorts = stationOutPorts(station);
    return station;
  };

  // ----------------------------------------------------------------- blocks
  Parser.prototype.parseBlock = function () {
    var tok = this.peek();
    if (tok.kind === "PUNCT" && tok.value === "{") {
      this.advance();
      var flows = this.parseFlowsUntil(function (t) {
        return t.kind === "PUNCT" && t.value === "}";
      });
      if (this.atPunct("}")) {
        var closer = this.advance();
        return node("Block", { start: tok.span.start, end: closer.span.end }, { flows: flows });
      }
      this.error("E0102", "missing `}` to close this block", this.peek().span);
      return node("Block", { start: tok.span.start, end: this.peek().span.end }, { flows: flows });
    }
    if (tok.kind === "KW" && (tok.value === "DO" || tok.value === "BEGIN")) {
      var opener = this.advance();
      var flows2 = this.parseFlowsUntil(function (t) {
        return t.kind === "KW" && t.value === "END";
      });
      if (this.atKw("END")) {
        var closer2 = this.advance();
        return node("Block", { start: opener.span.start, end: closer2.span.end }, { flows: flows2 });
      }
      this.error("E0102", "missing `end` to close this block", this.peek().span);
      return node("Block", { start: opener.span.start, end: this.peek().span.end }, { flows: flows2 });
    }
    this.error(
      "E0102",
      "expected a block: `{ \u2026 }`, or `do \u2026 end` / `begin \u2026 end`",
      tok.span,
      "a block is required here"
    );
    throw new Recovers();
  };

  Parser.prototype.parseFlowsUntil = function (stop) {
    var flows = [];
    this.skipSeps();
    while (!this.atEof() && !stop(this.peek())) {
      var before = this.i;
      try {
        var flow = this.parseFlow();
        if (flow) flows.push(flow);
      } catch (err) {
        if (!(err instanceof Recovers)) throw err;
        this.recover();
      }
      if (this.i === before) this.advance();
      this.skipSeps();
    }
    return flows;
  };

  // ------------------------------------------------------------------- flows
  Parser.prototype.parseFlow = function () {
    var tok = this.peek();
    if (tok.kind === "KW") {
      var value = tok.value;
      if (value === "EMIT") return this.parseEmit();
      if (value === "WHEN") return this.parseWhen();
      if (value === "CYCLE") return this.parseCycle();
      if (value === "FOR") return this.parseFor();
      if (value === "OUT") return this.parseOut();
      if (value === "RETURN") return this.parseReturn();
      if (value === "HALT") return node("Halt", this.advance().span, {});
      if (value === "SKIP") return node("Skip", this.advance().span, {});
      if (value === "OTHERWISE") {
        this.error(
          "E0100",
          "`otherwise` must directly follow a `when` block",
          tok.span,
          "use `when \u2026 { } otherwise { }`"
        );
        throw new Recovers();
      }
      if (value === "STATION" || value === "USE" || value === "WIRE") return this.parseItem();
      if (value === "END") {
        this.error("E0100", "unexpected `end`", tok.span);
        throw new Recovers();
      }
    }
    if (tok.kind === "IDENT" && this.peek(1).kind === "KW" && this.peek(1).value === "EMIT") {
      return this.parseEmitInfix();
    }
    var expr = this.parseExpr();
    var flow = node("Flow", { start: tok.span.start, end: expr.span.end }, { expr: expr });
    // `wenn cond { ... }` -- an identifier where a flow keyword belongs.
    if (tok.kind === "IDENT" && this.atBlockStart()) {
      var hint = this.keywordTypoHint(tok.value);
      if (hint !== null) {
        this.error("E0101", "unknown keyword `" + tok.value + "`", tok.span, hint);
        if (expr.type === "Call") expr.keyword_typo = true;
      }
    }
    return flow;
  };

  Parser.prototype.atBlockStart = function () {
    return this.atPunct("{") || this.atKw("DO", "BEGIN");
  };

  Parser.prototype.keywordTypoHint = function (name) {
    var matches = suggest(name, this.table.keywordWords, 1, 0.7);
    if (!matches.length) return null;
    var word = matches[0];
    var tokenName = this.table.isKeywordSpelling(word);
    var keyword = tokenName ? this.table.keywordByName[tokenName] : null;
    if (!keyword) return null;
    return "maybe you meant `" + word + "` (also written: " + keyword.spellings.join(" | ") + ")";
  };

  Parser.prototype.parseEmit = function () {
    var tok = this.expectKw("EMIT");
    var target = this.expectIdent();
    if (this.atPunct(":") || this.atPunct("=")) this.advance();
    else if (this.atKw("EMIT")) this.advance();
    if (this.atSep() || this.atEof() || this.atPunct("}")) {
      this.error("E0102", "`emit` needs a value", this.peek().span);
      throw new Recovers();
    }
    var value = this.parseExpr();
    return node("Emit", { start: tok.span.start, end: value.span.end }, {
      target: target.value,
      value: value
    });
  };

  Parser.prototype.parseEmitInfix = function () {
    var target = this.advance();
    this.advance();
    var value = this.parseExpr();
    return node("Emit", { start: target.span.start, end: value.span.end }, {
      target: target.value,
      value: value
    });
  };

  Parser.prototype.parseWhen = function () {
    var tok = this.expectKw("WHEN");
    var cond = this.parseExpr();
    if (this.atKw("THEN")) this.advance();
    var body = this.parseBlock();
    var otherwise = null;
    var end = body.span;
    this.skipSeps();
    if (this.atKw("OTHERWISE")) {
      this.advance();
      this.skipSeps();
      if (this.atKw("WHEN")) otherwise = this.parseWhen();
      else otherwise = this.parseBlock();
      end = otherwise.span;
    }
    return node("When", { start: tok.span.start, end: end.end }, {
      cond: cond,
      body: body,
      otherwise: otherwise
    });
  };

  Parser.prototype.parseCycle = function () {
    var tok = this.expectKw("CYCLE");
    var kind = "count";
    var head = null;
    if (this.atKw("WHILE")) {
      this.advance();
      kind = "while";
      head = this.parseExpr();
    } else if (this.atKw("UNTIL")) {
      this.advance();
      kind = "until";
      head = this.parseExpr();
    } else if (this.atKw("OVER")) {
      this.advance();
      kind = "over";
      head = this.parseExpr();
    } else {
      head = this.parseExpr();
    }
    var varName = null;
    if (this.atKw("AS")) {
      this.advance();
      varName = this.expectIdent().value;
    }
    var body = this.parseBlock();
    return node("Cycle", { start: tok.span.start, end: body.span.end }, {
      kind: kind,
      head: head,
      var: varName,
      body: body
    });
  };

  Parser.prototype.parseFor = function () {
    var tok = this.expectKw("FOR");
    var varName = this.expectIdent();
    this.expectKw("IN");
    var items = this.parseExpr();
    var body = this.parseBlock();
    return node("For", { start: tok.span.start, end: body.span.end }, {
      var: varName.value,
      items: items,
      body: body
    });
  };

  Parser.prototype.parseOut = function () {
    var tok = this.expectKw("OUT");
    var name = this.expectIdent();
    if (this.atPunct(":") || this.atPunct("=")) this.advance();
    if (this.atSep() || this.atEof() || this.atPunct("}")) {
      this.error("E0102", "output port `" + name.value + "` needs a value", this.peek().span);
      throw new Recovers();
    }
    var value = this.parseExpr();
    return node("Out", { start: tok.span.start, end: value.span.end }, {
      name: name.value,
      value: value
    });
  };

  Parser.prototype.parseReturn = function () {
    var tok = this.expectKw("RETURN");
    if (this.atSep() || this.atEof() || this.atPunct("}") || this.atKw("END")) {
      return node("Return", tok.span, { value: null });
    }
    var value = this.parseExpr();
    return node("Return", { start: tok.span.start, end: value.span.end }, { value: value });
  };

  // -------------------------------------------------------------- expressions
  Parser.prototype.parseExpr = function (minPrec) {
    if (minPrec === undefined) minPrec = 0;
    var left = this.parseUnary();
    for (;;) {
      var tok = this.peek();
      if (tok.kind !== "OP") break;
      var op = this.table.operatorByName[tok.value];
      if (!op || op.arity !== "infix") break;
      if (op.precedence < minPrec) break;
      this.advance();
      var nextMin = op.assoc === "right" ? op.precedence : op.precedence + 1;
      var right = this.parseExpr(nextMin);
      left = node("Binary", { start: left.span.start, end: right.span.end }, {
        op: op.name,
        left: left,
        right: right
      });
    }
    return left;
  };

  Parser.prototype.parseUnary = function () {
    var tok = this.peek();
    if (tok.kind === "OP" && (tok.value === "NOT" || tok.value === "MINUS")) {
      this.advance();
      var operand = this.parseUnary();
      return node("Unary", { start: tok.span.start, end: operand.span.end }, {
        op: tok.value,
        operand: operand
      });
    }
    return this.parsePostfix();
  };

  Parser.prototype.callableName = function (expr) {
    if (expr.type === "Name") return { name: expr.name, glyph: expr.glyph };
    if (expr.type === "Field" && expr.base.type === "Name" && !expr.base.glyph) {
      return { name: expr.base.name + "." + expr.name, glyph: false };
    }
    return null;
  };

  Parser.prototype.startsImplicitArg = function (tok) {
    if (EXPRESSION_START_KINDS[tok.kind]) return true;
    if (tok.kind === "PUNCT" && "([\u27e8".indexOf(tok.value) >= 0) return true;
    if (tok.kind === "KW" && (tok.value === "ASSEMBLY" || tok.value === "DRAW")) return true;
    return false;
  };

  Parser.prototype.parsePostfix = function () {
    var expr = this.parsePrimary();
    for (;;) {
      var tok = this.peek();
      if (tok.kind === "PUNCT" && tok.value === ".") {
        this.advance();
        var ident = this.fieldName();
        var nxt = this.peek();
        if (
          expr.type === "Name" &&
          !expr.glyph &&
          nxt.kind === "PUNCT" &&
          (nxt.value === "(" || nxt.value === ".")
        ) {
          expr = node("Name", { start: expr.span.start, end: ident.span.end }, {
            name: expr.name + "." + ident.text,
            glyph: false
          });
          continue;
        }
        expr = node("Field", { start: expr.span.start, end: ident.span.end }, {
          base: expr,
          name: ident.text
        });
        continue;
      }
      if (tok.kind === "PUNCT" && tok.value === "(") {
        var args = this.parseParenArgs();
        var end = this.tokens[this.i - 1].span;
        expr = this.makeCall(expr, args, { start: expr.span.start, end: end.end });
        continue;
      }
      var info = this.callableName(expr);
      if (info !== null && this.startsImplicitArg(tok)) {
        var implicitArgs = this.parseImplicitArgs();
        expr = this.makeCall(expr, implicitArgs, {
          start: expr.span.start,
          end: implicitArgs[implicitArgs.length - 1].value.span.end
        });
        continue;
      }
      break;
    }
    return expr;
  };

  Parser.prototype.makeCall = function (expr, args, span) {
    var info = this.callableName(expr);
    if (info !== null) {
      return node("Call", span, {
        name: info.name,
        callee: null,
        args: args,
        glyph: info.glyph,
        keyword_typo: false
      });
    }
    return node("Call", span, {
      name: null,
      callee: expr,
      args: args,
      glyph: false,
      keyword_typo: false
    });
  };

  Parser.prototype.parseParenArgs = function () {
    this.expectPunct("(");
    var args = [];
    if (this.atPunct(")")) {
      this.advance();
      return args;
    }
    for (;;) {
      if (this.atEof()) {
        this.error("E0102", "missing `)` to close this argument list", this.peek().span);
        break;
      }
      if (this.atPunct(")")) {
        this.advance();
        break;
      }
      args.push(this.parseArg());
      if (this.atPunct(",")) {
        this.advance();
        continue;
      }
      if (this.atPunct(")")) {
        this.advance();
        break;
      }
      if (this.startsImplicitArg(this.peek())) continue;
      this.error("E0103", "expected `,` or `)` in the argument list", this.peek().span);
      throw new Recovers();
    }
    return args;
  };

  Parser.prototype.parseImplicitArgs = function () {
    var args = [this.parseArg()];
    while (this.atPunct(",")) {
      this.advance();
      args.push(this.parseArg());
    }
    return args;
  };

  Parser.prototype.parseArg = function () {
    var tok = this.peek();
    if (tok.kind === "IDENT" && this.peek(1).kind === "PUNCT" && (this.peek(1).value === ":" || this.peek(1).value === "=")) {
      var label = this.advance();
      this.advance();
      var value = this.parseExpr();
      return node("Arg", { start: label.span.start, end: value.span.end }, {
        label: label.value,
        value: value
      });
    }
    var plain = this.parseExpr();
    return node("Arg", plain.span, { label: null, value: plain });
  };

  Parser.prototype.parsePrimary = function () {
    var tok = this.peek();
    if (tok.kind === "NUMBER") {
      this.advance();
      return node("NumberLit", tok.span, { value: tok.value });
    }
    if (tok.kind === "TEXT") {
      this.advance();
      return node("TextLit", tok.span, { value: tok.value });
    }
    if (tok.kind === "FLAG") {
      this.advance();
      return node("FlagLit", tok.span, { value: tok.value });
    }
    if (tok.kind === "NIL") {
      this.advance();
      return node("NilLit", tok.span, {});
    }
    if (tok.kind === "IDENT") {
      this.advance();
      return node("Name", tok.span, { name: tok.value, glyph: false });
    }
    if (tok.kind === "GLYPH") {
      this.advance();
      return node("Name", tok.span, { name: tok.value, glyph: true });
    }
    if (tok.kind === "PUNCT" && tok.value === "(") {
      this.advance();
      var expr = this.parseExpr();
      if (this.atPunct(")")) this.advance();
      else this.error("E0102", "missing `)`", this.peek().span);
      return expr;
    }
    if (tok.kind === "PUNCT" && tok.value === "[") return this.parseListLiteral("]");
    if (tok.kind === "PUNCT" && tok.value === "\u27e8") return this.parseListLiteral("\u27e9");
    if (tok.kind === "KW" && tok.value === "ASSEMBLY") return this.parseAssemblyLiteral();
    if (tok.kind === "KW" && tok.value === "DRAW") {
      this.advance();
      var name = this.expectIdent();
      return node("Read", { start: tok.span.start, end: name.span.end }, { name: name.value });
    }
    this.unexpected("an expression");
    throw new Recovers();
  };

  Parser.prototype.parseListLiteral = function (closer) {
    var opener = this.advance();
    var items = [];
    for (;;) {
      this.skipSeps();
      if (this.atEof()) {
        this.error("E0102", "missing `" + closer + "` to close this list", this.peek().span);
        break;
      }
      if (this.atPunct(closer)) {
        var end = this.advance();
        return node("ListLit", { start: opener.span.start, end: end.span.end }, { items: items });
      }
      items.push(this.parseExpr());
      if (this.atPunct(",")) {
        this.advance();
        continue;
      }
      this.skipSeps();
      if (this.atPunct(closer)) continue;
      if (this.startsImplicitArg(this.peek())) continue;
      this.error("E0103", "expected `,` or `" + closer + "` in this list", this.peek().span);
      throw new Recovers();
    }
    if (items.length) {
      return node("ListLit", { start: opener.span.start, end: this.peek().span.end }, { items: items });
    }
    throw new Recovers();
  };

  Parser.prototype.keyName = function () {
    var tok = this.peek();
    if (tok.kind === "IDENT") return this.advance();
    if (
      (tok.kind === "KW" || tok.kind === "OP" || tok.kind === "FLAG" || tok.kind === "NIL") &&
      tok.text &&
      /^[\p{L}\p{N}_]+$/u.test(tok.text)
    ) {
      this.advance();
      return { kind: "IDENT", value: tok.text, text: tok.text, span: tok.span };
    }
    return this.expectIdent();
  };

  Parser.prototype.fieldName = function () {
    var tok = this.peek();
    if (tok.kind === "IDENT") return this.advance();
    // Tolerate keywords/operators as member names: store.read, json.text, …
    if (
      (tok.kind === "KW" || tok.kind === "OP" || tok.kind === "FLAG" || tok.kind === "NIL") &&
      tok.text &&
      /^[\p{L}\p{N}_]+$/u.test(tok.text)
    ) {
      this.advance();
      return { kind: "IDENT", value: tok.text, text: tok.text, span: tok.span };
    }
    this.error("E0100", "expected a field name after `.`", tok.span);
    throw new Recovers();
  };

  Parser.prototype.parseAssemblyLiteral = function () {
    var tok = this.advance();
    if (!this.atPunct("{")) {
      this.error("E0100", "expected `{` after `assembly`", this.peek().span);
      throw new Recovers();
    }
    this.advance();
    var fields = [];
    for (;;) {
      this.skipSeps();
      if (this.atEof()) {
        this.error("E0102", "missing `}` to close this assembly", this.peek().span);
        break;
      }
      if (this.atPunct("}")) {
        var end = this.advance();
        return node("AssemblyLit", { start: tok.span.start, end: end.span.end }, { fields: fields });
      }
      var name = this.keyName();
      if (this.atPunct(":") || this.atPunct("=")) this.advance();
      var value = this.parseExpr();
      fields.push(node("FieldInit", { start: name.span.start, end: value.span.end }, {
        name: name.text,
        value: value
      }));
      if (this.atPunct(",")) this.advance();
    }
    return node("AssemblyLit", { start: tok.span.start, end: this.peek().span.end }, { fields: fields });
  };

  // --------------------------------------------------------------- utilities
  function walkFlows(block) {
    var out = [];
    function visitFlows(flows) {
      for (var i = 0; i < flows.length; i += 1) {
        var flow = flows[i];
        out.push(flow);
        if (flow.type === "When") {
          visitFlows(flow.body.flows);
          if (flow.otherwise) {
            if (flow.otherwise.type === "When") visitFlows([flow.otherwise]);
            else visitFlows(flow.otherwise.flows);
          }
        } else if (flow.type === "Cycle" || flow.type === "For") {
          visitFlows(flow.body.flows);
        }
      }
    }
    visitFlows(block.flows);
    return out;
  }

  function stationOutPorts(station) {
    var names = [];
    var flows = walkFlows(station.body);
    for (var i = 0; i < flows.length; i += 1) {
      var flow = flows[i];
      if (flow.type === "Out" && names.indexOf(flow.name) < 0) names.push(flow.name);
      else if (flow.type === "Return" && flow.value !== null && names.indexOf("value") < 0) names.push("value");
    }
    return names;
  }

  MI.parse = function (tokens) {
    var parser = new Parser(tokens);
    var program = parser.parseProgram();
    return { program: program, diagnostics: parser.diagnostics };
  };
  MI.stationOutPorts = stationOutPorts;
  MI._internals = MI._internals || {};
  MI._internals.walkFlows = walkFlows;
  MI._internals.pyRepr = pyRepr;
})();
