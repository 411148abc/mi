/* MI browser runtime -- lexer.
 *
 * Classic script: attaches MI.lex(source) -> {tokens, diagnostics}.
 * Mirrors 米/lexer.py and is driven by window.MI_GLYPHS (generated from
 * 规格/glyphs.json), so every spelling is matched identically in both runtimes.
 */
(function () {
  "use strict";

  window.MI = window.MI || {};
  var MI = window.MI;

  var WORD_ESCAPES = {
    n: "\n",
    t: "\t",
    r: "\r",
    "\\": "\\",
    '"': '"',
    "'": "'",
    "0": "\u0000",
    "\u00ab": "\u00ab", // «
    "\u00bb": "\u00bb" // »
  };
  var QUOTE_CLOSERS = { '"': '"', "'": "'", "\u00ab": "\u00bb" };

  var PUNCTUATION = "(){}[]\u27e8\u27e9,:.;="; // (){}[]⟨⟩,:.;=

  function isWordChar(ch) {
    return !!ch && /[\p{L}\p{N}_]/u.test(ch);
  }

  function position(line, col, offset) {
    return { line: line, col: col, offset: offset };
  }

  function makeSpan(start, end) {
    return { start: start, end: end };
  }

  function diag(severity, code, message, span, hint) {
    return {
      severity: severity,
      code: code,
      message: message,
      span: span,
      hint: hint === undefined ? null : hint
    };
  }

  // -------------------------------------------------------------- glyph table
  var TABLE = null;

  function glyphTable() {
    if (TABLE) return TABLE;
    var G = window.MI_GLYPHS;
    if (!G) throw new Error("MI_GLYPHS missing: load 网页浏览器/js/glyphs.js first");
    var table = {
      keywords: [],
      operators: [],
      literals: [],
      builtins: [],
      keywordByName: {},
      operatorByName: {},
      literalByName: {},
      keywordByLower: {},
      operatorByLower: {},
      literalByLower: {},
      keywordBySymbol: {},
      operatorBySymbol: {},
      literalBySymbol: {},
      builtinByCanonical: {},
      builtinAllAliases: {},
      builtinWordAliases: {},
      builtinSymbolAliases: {},
      matchEntries: [],
      suggestionWords: []
    };
    function isWordSpelling(sp) {
      if (!/^[\p{L}\p{N}_]+$/u.test(sp)) return false;
      return /[\p{L}_]/u.test(sp);
    }
    function isSymbolSpelling(sp) {
      return !/[\p{L}\p{N}_]/u.test(sp);
    }
    (G.keywords || []).forEach(function (k) {
      table.keywords.push(k);
      table.keywordByName[k.name] = k;
      (k.spellings || []).forEach(function (sp) {
        if (isWordSpelling(sp)) table.keywordByLower[sp.toLowerCase()] = k.name;
        else table.keywordBySymbol[sp] = k.name;
      });
    });
    (G.operators || []).forEach(function (o) {
      table.operators.push(o);
      table.operatorByName[o.name] = o;
      (o.spellings || []).forEach(function (sp) {
        if (isWordSpelling(sp)) table.operatorByLower[sp.toLowerCase()] = o.name;
        else table.operatorBySymbol[sp] = o.name;
      });
    });
    (G.literals || []).forEach(function (l) {
      table.literals.push(l);
      table.literalByName[l.name] = l;
      (l.spellings || []).forEach(function (sp) {
        if (isWordSpelling(sp)) table.literalByLower[sp.toLowerCase()] = l.name;
        else table.literalBySymbol[sp] = l.name;
      });
    });
    (G.builtins || []).forEach(function (b) {
      table.builtins.push(b);
      table.builtinByCanonical[b.canonical] = b;
      (b.spellings || []).forEach(function (sp) {
        // Every spelling (dotted spellings included) resolves case-insensitively.
        table.builtinAllAliases[sp.toLowerCase()] = b.canonical;
        if (sp.indexOf(".") >= 0) return; // ui.mount etc. are parser-level names
        if (isWordSpelling(sp)) table.builtinWordAliases[sp.toLowerCase()] = b.canonical;
        else if (isSymbolSpelling(sp)) table.builtinSymbolAliases[sp] = b.canonical;
      });
    });
    // Longest match first; ties broken by spelling text (stable + deterministic).
    var entries = [];
    Object.keys(table.keywordBySymbol).forEach(function (sp) {
      entries.push({ spelling: sp, isWord: false, kind: "KW", value: table.keywordBySymbol[sp] });
    });
    Object.keys(table.operatorBySymbol).forEach(function (sp) {
      entries.push({ spelling: sp, isWord: false, kind: "OP", value: table.operatorBySymbol[sp] });
    });
    Object.keys(table.literalBySymbol).forEach(function (sp) {
      entries.push({ spelling: sp, isWord: false, kind: "LITERAL", value: table.literalBySymbol[sp] });
    });
    Object.keys(table.builtinSymbolAliases).forEach(function (sp) {
      entries.push({ spelling: sp, isWord: false, kind: "GLYPH", value: table.builtinSymbolAliases[sp] });
    });
    Object.keys(table.keywordByLower).forEach(function (sp) {
      entries.push({ spelling: sp, isWord: true, kind: "KW", value: table.keywordByLower[sp] });
    });
    Object.keys(table.operatorByLower).forEach(function (sp) {
      entries.push({ spelling: sp, isWord: true, kind: "OP", value: table.operatorByLower[sp] });
    });
    Object.keys(table.literalByLower).forEach(function (sp) {
      entries.push({ spelling: sp, isWord: true, kind: "LITERAL", value: table.literalByLower[sp] });
    });
    entries.sort(function (a, b) {
      if (a.spelling.length !== b.spelling.length) return b.spelling.length - a.spelling.length;
      return a.spelling < b.spelling ? -1 : a.spelling > b.spelling ? 1 : 0;
    });
    table.matchEntries = entries;
    table.maxSpellingLen = entries.reduce(function (n, e) {
      return Math.max(n, e.spelling.length);
    }, 1);
    var words = {};
    var keywordWords = {};
    table.keywords.forEach(function (k) {
      k.spellings.forEach(function (sp) {
        if (!isWordSpelling(sp)) return;
        words[sp.toLowerCase()] = true;
        keywordWords[sp.toLowerCase()] = true;
      });
    });
    table.operators.forEach(function (o) {
      o.spellings.forEach(function (sp) {
        if (isWordSpelling(sp)) words[sp.toLowerCase()] = true;
      });
    });
    table.literals.forEach(function (l) {
      l.spellings.forEach(function (sp) {
        if (isWordSpelling(sp)) words[sp.toLowerCase()] = true;
      });
    });
    Object.keys(table.builtinWordAliases).forEach(function (w) {
      words[w] = true;
    });
    table.suggestionWords = Object.keys(words).sort();
    table.keywordWords = Object.keys(keywordWords).sort();
    table.builtin = function (name) {
      if (!name) return null;
      if (Object.prototype.hasOwnProperty.call(table.builtinByCanonical, name)) {
        return table.builtinByCanonical[name];
      }
      var low = String(name).toLowerCase();
      var canonical = table.builtinAllAliases[low];
      if (canonical) return table.builtinByCanonical[canonical];
      return null;
    };
    table.isKeywordSpelling = function (text) {
      if (!text) return null;
      var key = isWordSpelling(text) ? text.toLowerCase() : text;
      if (Object.prototype.hasOwnProperty.call(table.keywordByLower, key)) return table.keywordByLower[key];
      if (Object.prototype.hasOwnProperty.call(table.keywordBySymbol, key)) return table.keywordBySymbol[key];
      return null;
    };
    table.builtinByCanonicalName = function (name) {
      return table.builtinByCanonical[name] || null;
    };
    TABLE = table;
    return table;
  }

  // ------------------------------------------------------------------- lexer
  function Lexer(source, filename) {
    this.source = source;
    this.filename = filename || "<source>";
    this.pos = 0;
    this.line = 1;
    this.col = 1;
    this.tokens = [];
    this.diagnostics = [];
    this.table = glyphTable();
  }

  Lexer.prototype.here = function () {
    return position(this.line, this.col, this.pos);
  };

  Lexer.prototype.peek = function (k) {
    var idx = this.pos + (k || 0);
    return idx < this.source.length ? this.source.charAt(idx) : "";
  };

  Lexer.prototype.advance = function () {
    var ch = this.source.charAt(this.pos);
    this.pos += 1;
    if (ch === "\n") {
      this.line += 1;
      this.col = 1;
    } else {
      this.col += 1;
    }
    return ch;
  };

  Lexer.prototype.emit = function (kind, value, start) {
    var text = this.source.slice(start.offset, this.pos);
    var span = makeSpan(start, this.here());
    if (kind === "SEP" && this.tokens.length && this.tokens[this.tokens.length - 1].kind === "SEP") {
      return;
    }
    this.tokens.push({ kind: kind, value: value, text: text, span: span });
  };

  Lexer.prototype.error = function (code, message, start, hint) {
    var end =
      this.pos > start.offset
        ? this.here()
        : position(start.line, start.col + 1, start.offset + 1);
    this.diagnostics.push(diag("error", code, message, makeSpan(start, end), hint || null));
  };

  Lexer.prototype.run = function () {
    var source = this.source;
    while (this.pos < source.length) {
      var ch = this.peek();
      if (ch === " " || ch === "\t" || ch === "\r") {
        this.advance();
        continue;
      }
      if (ch === "\n") {
        var sepStart = this.here();
        this.advance();
        this.emit("SEP", "\n", sepStart);
        continue;
      }
      if (this.peek() === ";" && this.peek(1) === ";") {
        this.skipLineComment();
        continue;
      }
      if (ch === ";") {
        var semiStart = this.here();
        this.advance();
        this.emit("SEP", ";", semiStart);
        continue;
      }
      if (ch === "#") {
        this.skipLineComment();
        continue;
      }
      if (this.peek() === "-" && this.peek(1) === "-") {
        this.skipLineComment();
        continue;
      }
      if (this.peek() === "/" && this.peek(1) === "*") {
        this.blockComment("/*", "*/");
        continue;
      }
      if (this.peek() === "(" && this.peek(1) === "*") {
        this.blockComment("(*", "*)");
        continue;
      }
      if (QUOTE_CLOSERS[ch]) {
        this.lexText();
        continue;
      }
      if (/\d/.test(ch) || (ch === "." && /\d/.test(this.peek(1)))) {
        this.lexNumber();
        continue;
      }
      if (this.matchSpelling()) continue;
      if (PUNCTUATION.indexOf(ch) >= 0) {
        var punctStart = this.here();
        this.advance();
        this.emit("PUNCT", ch, punctStart);
        continue;
      }
      if (isWordChar(ch)) {
        this.lexIdentifier();
        continue;
      }
      var badStart = this.here();
      this.advance();
      this.error("E0001", "unexpected character '" + ch + "'", badStart);
    }

    if (!this.tokens.length || this.tokens[this.tokens.length - 1].kind !== "SEP") {
      var here = this.here();
      this.tokens.push({ kind: "SEP", value: "\n", text: "", span: makeSpan(here, here) });
    }
    var end = this.here();
    this.tokens.push({ kind: "EOF", value: null, text: "", span: makeSpan(end, end) });
    return { tokens: this.tokens, diagnostics: this.diagnostics };
  };

  Lexer.prototype.skipLineComment = function () {
    while (this.pos < this.source.length && this.peek() !== "\n") this.advance();
  };

  Lexer.prototype.blockComment = function (opener, closer) {
    var start = this.here();
    this.advance();
    this.advance();
    while (this.pos < this.source.length) {
      if (this.peek() === closer.charAt(0) && this.peek(1) === closer.charAt(1)) {
        this.advance();
        this.advance();
        return;
      }
      this.advance();
    }
    this.error("E0002", "unterminated block comment, expected '" + closer + "'", start);
  };

  Lexer.prototype.lexText = function () {
    var start = this.here();
    var opener = this.advance();
    var closer = QUOTE_CLOSERS[opener];
    var value = [];
    for (;;) {
      if (this.pos >= this.source.length) {
        this.error("E0002", "unterminated text literal", start);
        break;
      }
      var ch = this.advance();
      if (ch === closer) break;
      if (ch === "\\") {
        if (this.pos >= this.source.length) {
          this.error("E0003", "text literal ends with a dangling escape", start);
          break;
        }
        var esc = this.advance();
        if (Object.prototype.hasOwnProperty.call(WORD_ESCAPES, esc)) {
          value.push(WORD_ESCAPES[esc]);
        } else if (esc === "u") {
          if (this.peek() !== "{") {
            this.error("E0003", "expected `{` after `\\u`", start);
            continue;
          }
          this.advance();
          var digits = "";
          while (this.pos < this.source.length && this.peek() !== "}") digits += this.advance();
          if (this.peek() === "}") this.advance();
          var codePoint = parseInt(digits, 16);
          if (!digits || isNaN(codePoint) || codePoint > 0x10ffff) {
            this.error("E0003", "bad unicode escape `\\u{" + digits + "}`", start);
          } else {
            value.push(String.fromCodePoint(codePoint));
          }
        } else if (esc === "\n") {
          continue;
        } else {
          this.error("E0003", "unknown escape `\\" + esc + "`", start, "use \\\\ for a backslash");
          value.push(esc);
        }
      } else {
        value.push(ch);
      }
    }
    this.emit("TEXT", value.join(""), start);
  };

  Lexer.prototype.lexNumber = function () {
    var start = this.here();
    var isInt = true;
    var value;
    if (this.peek() === "0" && (this.peek(1) === "x" || this.peek(1) === "X")) {
      this.advance();
      this.advance();
      var hex = "";
      while (/[0-9A-Za-z_]/.test(this.peek()) && this.peek() !== "") hex += this.advance();
      var cleanHex = hex.replace(/_/g, "");
      value = parseInt(cleanHex, 16);
      if (isNaN(value) || !/^[0-9a-fA-F]+$/.test(cleanHex)) {
        this.error("E0004", "bad hexadecimal number `0x" + hex + "`", start);
        value = 0;
      }
      this.emit("NUMBER", value, start);
      this.tokens[this.tokens.length - 1].isInt = true;
      return;
    }
    if (this.peek() === "0" && (this.peek(1) === "b" || this.peek(1) === "B")) {
      this.advance();
      this.advance();
      var bin = "";
      while (/[0-9A-Za-z_]/.test(this.peek()) && this.peek() !== "") bin += this.advance();
      var cleanBin = bin.replace(/_/g, "");
      value = parseInt(cleanBin, 2);
      if (isNaN(value) || !/^[01]+$/.test(cleanBin)) {
        this.error("E0004", "bad binary number `0b" + bin + "`", start);
        value = 0;
      }
      this.emit("NUMBER", value, start);
      this.tokens[this.tokens.length - 1].isInt = true;
      return;
    }

    var text = "";
    var seenDot = false;
    for (;;) {
      var ch = this.peek();
      if (/\d/.test(ch)) {
        text += this.advance();
      } else if (ch === "_" && text && /\d/.test(this.peek(1))) {
        text += this.advance();
      } else if (ch === "." && !seenDot && /\d/.test(this.peek(1))) {
        seenDot = true;
        text += this.advance();
      } else {
        break;
      }
    }
    if (this.peek() === "e" || this.peek() === "E") {
      var savePos = this.pos;
      var saveLine = this.line;
      var saveCol = this.col;
      var exp = this.advance();
      if (this.peek() === "+" || this.peek() === "-") exp += this.advance();
      var digits = "";
      while (/\d/.test(this.peek())) digits += this.advance();
      if (digits) {
        text += exp + digits;
        isInt = false;
      } else {
        this.pos = savePos;
        this.line = saveLine;
        this.col = saveCol;
        this.error("E0004", "bad number: exponent has no digits", start);
      }
    }
    var clean = text.replace(/_/g, "");
    if (clean.indexOf(".") >= 0 || clean.indexOf("e") >= 0 || clean.indexOf("E") >= 0) {
      isInt = false;
      value = parseFloat(clean);
    } else {
      value = parseInt(clean, 10);
    }
    if (isNaN(value)) {
      this.error("E0004", "bad number `" + text + "`", start);
      value = 0;
    }
    this.emit("NUMBER", value, start);
    this.tokens[this.tokens.length - 1].isInt = isInt;
  };

  Lexer.prototype.lexIdentifier = function () {
    var start = this.here();
    var text = "";
    while (isWordChar(this.peek())) text += this.advance();
    this.emit("IDENT", text, start);
  };

  Lexer.prototype.matchSpelling = function () {
    var source = this.source;
    var startPos = this.pos;
    var entries = this.table.matchEntries;
    for (var i = 0; i < entries.length; i += 1) {
      var entry = entries[i];
      var spelling = entry.spelling;
      var length = spelling.length;
      if (startPos + length > source.length) continue;
      var windowText = source.substr(startPos, length);
      if (entry.isWord) {
        if (windowText.toLowerCase() !== spelling) continue;
        var after = startPos + length < source.length ? source.charAt(startPos + length) : "";
        if (isWordChar(after)) continue;
        if (startPos > 0 && isWordChar(source.charAt(startPos - 1))) continue;
      } else if (windowText !== spelling) {
        continue;
      }
      var start = this.here();
      for (var k = 0; k < length; k += 1) this.advance();
      if (entry.kind === "LITERAL") {
        this.emitLiteral(entry.value, start);
      } else {
        this.emit(entry.kind, entry.value, start);
      }
      return true;
    }
    return false;
  };

  Lexer.prototype.emitLiteral = function (name, start) {
    var literal = null;
    for (var i = 0; i < this.table.literals.length; i += 1) {
      if (this.table.literals[i].name === name) {
        literal = this.table.literals[i];
        break;
      }
    }
    if (!literal) return;
    if (literal.kind === "flag") this.emit("FLAG", !!literal.value, start);
    else if (literal.kind === "nil") this.emit("NIL", null, start);
    else this.emit("TEXT", literal.value, start);
  };

  MI.lex = function (source, filename) {
    return new Lexer(source, filename).run();
  };
  MI.glyphTable = glyphTable;
  MI._internals = MI._internals || {};
  MI._internals.diag = diag;
  MI._internals.makeSpan = makeSpan;
  MI._internals.position = position;
  MI._internals.isWordChar = isWordChar;
})();
