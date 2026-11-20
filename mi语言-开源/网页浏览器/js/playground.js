/* Mi Browser — a self-contained IDE for the MI language.
 *
 * Classic script, no modules, no network: this file only touches the DOM that
 * index.html provides and the data/runtimes loaded before it:
 *   window.MI_GLYPHS, window.MI_EXAMPLES, window.MI_CONFORMANCE, window.MI.
 */
(function () {
  "use strict";

  var MI = window.MI;
  if (!MI) {
    document.body.textContent = "Mi Browser failed to load: window.MI is missing.";
    return;
  }

  var STORAGE_KEY = "米.浏览器.source.v1";
  var DEFAULT_SOURCE = [
    "-- Welcome to Mi Browser. Press Ctrl+Enter (or Run) to execute.",
    "-- 米: signals, not statements.",
    "",
    "~total: 0",
    "cycle 3 as i {",
    "    emit total: draw total + i",
    "}",
    "show(\"sum:\", draw total)",
    "",
    "station greet(name) {",
    "    out value: \"hello, \" + name",
    "}",
    "show(greet(\"mi\"))",
    "",
    "station onPress(payload) {",
    "    show(\"button\", payload.id, \"was pressed\")",
    "}",
    "",
    "ui.mount()",
    "ui.heading(\"Mi Browser\")",
    "ui.text(\"Edit the program on the left, then press Run.\")",
    "ui.button(\"Say hello\", onPress)"
  ].join("\n");

  // Virtual files for `use`: the examples tree doubles as an embedded module
  // catalogue so 示例/modules/main.mi runs from file:// untouched.
  var EXAMPLE_FILES = {};
  var EXAMPLES = Array.isArray(window.MI_EXAMPLES) ? window.MI_EXAMPLES : [];
  EXAMPLES.forEach(function (example) {
    if (example && typeof example.path === "string") {
      EXAMPLE_FILES[example.path] = example.source;
    }
  });

  var state = {
    uiHost: null,
    running: false,
    lastRunMs: 0,
    saveTimer: null
  };

  function $(id) {
    return document.getElementById(id);
  }

  function el(tag, className, textContent) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (textContent !== undefined && textContent !== null) node.textContent = String(textContent);
    return node;
  }

  // ------------------------------------------------------------------ tabs
  function activateTab(name) {
    var tabs = document.querySelectorAll(".tab");
    for (var i = 0; i < tabs.length; i += 1) {
      var isActive = tabs[i].getAttribute("data-tab") === name;
      tabs[i].classList.toggle("active", isActive);
      tabs[i].setAttribute("aria-selected", isActive ? "true" : "false");
    }
    var panels = document.querySelectorAll(".tab-panel");
    for (var j = 0; j < panels.length; j += 1) {
      panels[j].classList.toggle("active", panels[j].id === "panel-" + name);
    }
  }

  function wireTabs() {
    var tabs = document.querySelectorAll(".tab");
    for (var i = 0; i < tabs.length; i += 1) {
      tabs[i].addEventListener("click", function () {
        activateTab(this.getAttribute("data-tab"));
      });
    }
  }

  // --------------------------------------------------------------- storage
  function loadSavedSource() {
    try {
      var saved = window.localStorage.getItem(STORAGE_KEY);
      if (typeof saved === "string" && saved.length) return saved;
    } catch (err) {
      /* localStorage can be unavailable on some file:// configurations */
    }
    return null;
  }

  function persistSource(source) {
    if (state.saveTimer !== null) clearTimeout(state.saveTimer);
    state.saveTimer = setTimeout(function () {
      try {
        window.localStorage.setItem(STORAGE_KEY, source);
      } catch (err) {
        /* ignore quota/security errors */
      }
    }, 200);
  }

  // ----------------------------------------------------------------- editor
  function setEditorSource(source, moveCaretToStart) {
    var editor = $("editor");
    editor.value = source;
    if (moveCaretToStart) {
      editor.selectionStart = 0;
      editor.selectionEnd = 0;
      editor.scrollTop = 0;
    }
    persistSource(editor.value);
  }

  function insertAtCursor(text) {
    var editor = $("editor");
    var start = editor.selectionStart;
    var end = editor.selectionEnd;
    editor.focus();
    if (typeof editor.setRangeText === "function") {
      editor.setRangeText(text, start, end, "end");
    } else {
      var before = editor.value.slice(0, start);
      var after = editor.value.slice(end);
      editor.value = before + text + after;
      editor.selectionStart = editor.selectionEnd = start + text.length;
    }
    editor.dispatchEvent(new Event("input", { bubbles: true }));
  }

  // Word spellings read better with spaces around them; symbol spellings do not.
  function spellingInsertionText(spelling) {
    var isWord = /^[\p{L}\p{N}_]+$/u.test(spelling);
    return isWord ? " " + spelling + " " : spelling;
  }

  function wireEditor() {
    var editor = $("editor");
    editor.addEventListener("input", function () {
      persistSource(editor.value);
      setStatus("edited");
    });
    editor.addEventListener("keydown", function (event) {
      if (event.key === "Tab") {
        event.preventDefault();
        var start = editor.selectionStart;
        if (event.shiftKey) {
          // Unindent: remove up to two leading spaces of the current line.
          var lineStart = editor.value.lastIndexOf("\n", start - 1) + 1;
          var head = editor.value.slice(lineStart, lineStart + 2);
          var remove = 0;
          if (head.charAt(0) === " ") remove += 1;
          if (head.charAt(1) === " ") remove += 1;
          if (remove > 0) {
            editor.setRangeText("", lineStart, lineStart + remove, "preserve");
            editor.selectionStart = editor.selectionEnd = Math.max(lineStart, start - remove);
          }
          editor.dispatchEvent(new Event("input", { bubbles: true }));
          return;
        }
        editor.setRangeText("  ", start, editor.selectionEnd, "end");
        editor.dispatchEvent(new Event("input", { bubbles: true }));
        return;
      }
      if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
        event.preventDefault();
        runProgram();
      }
    });
  }

  // --------------------------------------------------------------- console
  function clearConsole() {
    $("console-output").textContent = "";
  }

  function appendConsoleLine(text) {
    var out = $("console-output");
    out.appendChild(el("span", "console-line", text));
    out.appendChild(document.createTextNode("\n"));
    out.scrollTop = out.scrollHeight;
  }

  function setStatus(text) {
    $("editor-status").textContent = text;
  }

  // ----------------------------------------------------------- diagnostics
  function renderDiagnostics(diagnostics, source) {
    var container = $("diagnostics");
    container.textContent = "";
    var errors = 0;
    var warnings = 0;
    var sourceLines = String(source || "").split("\n");
    for (var i = 0; i < diagnostics.length; i += 1) {
      var diagnostic = diagnostics[i];
      if (diagnostic.severity === "error") errors += 1;
      else warnings += 1;
      container.appendChild(renderDiagnostic(diagnostic, sourceLines));
    }
    $("diag-summary").textContent =
      diagnostics.length === 0
        ? "no diagnostics"
        : errors + " error(s), " + warnings + " warning(s)";
    return { errors: errors, warnings: warnings };
  }

  function renderDiagnostic(diagnostic, sourceLines) {
    var box = el("div", "diagnostic " + (diagnostic.severity === "error" ? "is-error" : "is-warning"));
    var head = el("div", "diagnostic-head");
    head.appendChild(el("span", "diagnostic-code", diagnostic.severity + "[" + diagnostic.code + "]"));
    head.appendChild(el("span", "diagnostic-message", diagnostic.message));
    box.appendChild(head);

    var start = diagnostic.span.start;
    var end = diagnostic.span.end;
    var location = el("div", "diagnostic-location", "line " + start.line + ":" + start.col);
    box.appendChild(location);

    var lineText = sourceLines[start.line - 1] !== undefined ? sourceLines[start.line - 1] : "";
    var excerpt = el("pre", "diagnostic-excerpt");
    var caretLength;
    if (end.line === start.line) caretLength = Math.max(1, end.col - start.col);
    else caretLength = 1;
    excerpt.textContent = lineText + "\n" + " ".repeat(Math.max(0, start.col - 1)) + "^".repeat(caretLength);
    box.appendChild(excerpt);

    if (diagnostic.hint) {
      box.appendChild(el("div", "diagnostic-hint", "hint: " + diagnostic.hint));
    }
    return box;
  }

  // -------------------------------------------------------------- examples
  function exampleDisplayName(example) {
    return example.name || example.path;
  }

  function wireExamples() {
    var select = $("example-select");
    select.appendChild(new Option("— custom program —", ""));
    for (var i = 0; i < EXAMPLES.length; i += 1) {
      var example = EXAMPLES[i];
      select.appendChild(new Option(exampleDisplayName(example), String(i)));
    }
    select.addEventListener("change", function () {
      if (select.value === "") return;
      var index = parseInt(select.value, 10);
      var example = EXAMPLES[index];
      if (!example) return;
      setEditorSource(example.source, true);
      setStatus("loaded " + example.path);
      activateTab("console");
      runProgram();
    });
  }

  function exampleFilesForRun() {
    return EXAMPLE_FILES;
  }

  // ------------------------------------------------------------- app opener
  // Fetch an MI application source (usually the frontend served by an MI
  // backend, e.g. http://127.0.0.1:8080/app/forum.mi) and run it in the page.
  function fetchAppSource(url) {
    var xhr = new XMLHttpRequest();
    try {
      xhr.open("GET", url, false);
      xhr.send(null);
    } catch (err) {
      throw new Error(err && err.message ? err.message : String(err));
    }
    if (xhr.status === 0) {
      throw new Error(
        "network error (is the backend running, and does it send CORS headers?)"
      );
    }
    if (xhr.status < 200 || xhr.status >= 300) {
      throw new Error("HTTP " + xhr.status + (xhr.statusText ? " " + xhr.statusText : ""));
    }
    return xhr.responseText;
  }

  function setAppStatus(text) {
    var status = $("app-status");
    if (status) status.textContent = text;
  }

  function openApp() {
    var input = $("app-url");
    var url = (input && input.value ? input.value : "").trim();
    if (!url) {
      setAppStatus("enter the URL of an MI source file");
      return;
    }
    setAppStatus("fetching " + url + " …");
    setStatus("fetching app…");
    var source;
    try {
      source = fetchAppSource(url);
    } catch (err) {
      var message = err && err.message ? err.message : String(err);
      setAppStatus("fetch failed: " + message);
      setStatus("fetch failed");
      renderDiagnostics(
        [
          {
            severity: "error",
            code: "E0309",
            message: "cannot fetch app source from " + url + ": " + message,
            span: {
              start: { line: 1, col: 1, offset: 0 },
              end: { line: 1, col: 2, offset: 1 }
            },
            hint: "start an MI backend first, e.g. python -m 米 run 应用/forum/backend.mi"
          }
        ],
        ""
      );
      activateTab("console");
      return;
    }
    setEditorSource(source, true);
    $("example-select").value = "";
    setAppStatus("loaded " + url);
    setStatus("loaded app " + url);
    activateTab("ui");
    runProgram();
  }

  function wireAppBar() {
    var openButton = $("app-open");
    if (openButton) openButton.addEventListener("click", openApp);
    var input = $("app-url");
    if (input) {
      input.addEventListener("keydown", function (event) {
        if (event.key === "Enter") {
          event.preventDefault();
          openApp();
        }
      });
    }
  }

  // --------------------------------------------------------------- palette
  function wirePalette() {
    var palette = $("glyph-palette");
    if (!window.MI_GLYPHS) return;
    var groups = [
      { key: "keywords", title: "keywords" },
      { key: "operators", title: "operators" },
      { key: "literals", title: "literals" },
      { key: "builtins", title: "builtins" }
    ];
    groups.forEach(function (group) {
      var entries = window.MI_GLYPHS[group.key] || [];
      if (!entries.length) return;
      var section = el("div", "palette-group");
      section.appendChild(el("h3", "palette-title", group.title));
      var chips = el("div", "palette-chips");
      entries.forEach(function (entry) {
        var spellings = entry.spellings || [];
        spellings.forEach(function (spelling) {
          var chip = el("button", "chip", spelling);
          chip.type = "button";
          chip.title =
            (entry.canonical || entry.name || "") +
            (entry.summary ? " — " + entry.summary : "");
          chip.addEventListener("click", function () {
            insertAtCursor(spellingInsertionText(spelling));
          });
          chips.appendChild(chip);
        });
      });
      section.appendChild(chips);
      palette.appendChild(section);
    });
  }

  // ------------------------------------------------------------- reference
  function buildReferenceRows() {
    var glyphs = window.MI_GLYPHS || {};
    var rows = [];
    function push(group, entries) {
      (entries || []).forEach(function (entry) {
        rows.push({
          group: group,
          canonical: entry.canonical || entry.name || "",
          spellings: (entry.spellings || []).join("  "),
          summary: entry.summary || ""
        });
      });
    }
    push("keyword", glyphs.keywords);
    push("operator", glyphs.operators);
    push("literal", glyphs.literals);
    push("builtin", glyphs.builtins);
    return rows;
  }

  function renderReference(filter) {
    var table = $("reference-table");
    var rows = buildReferenceRows();
    var needle = (filter || "").trim().toLowerCase();
    var shown = rows.filter(function (row) {
      if (!needle) return true;
      return (
        row.group.toLowerCase().indexOf(needle) >= 0 ||
        row.canonical.toLowerCase().indexOf(needle) >= 0 ||
        row.spellings.toLowerCase().indexOf(needle) >= 0
      );
    });
    table.textContent = "";
    $("reference-count").textContent = shown.length + " of " + rows.length + " entries";
    var header = el("div", "reference-row reference-head");
    ["group", "canonical", "spellings", "summary"].forEach(function (label) {
      header.appendChild(el("span", null, label));
    });
    table.appendChild(header);
    shown.forEach(function (row) {
      var line = el("div", "reference-row");
      line.appendChild(el("span", "reference-group", row.group));
      line.appendChild(el("span", "reference-canonical", row.canonical));
      line.appendChild(el("span", "reference-spellings", row.spellings));
      line.appendChild(el("span", "reference-summary", row.summary));
      table.appendChild(line);
    });
  }

  function wireReference() {
    var filter = $("reference-filter");
    filter.addEventListener("input", function () {
      renderReference(filter.value);
    });
    renderReference("");
  }

  // -------------------------------------------------------------- self-test
  function errorPairs(diagnostics) {
    var pairs = [];
    for (var i = 0; i < diagnostics.length; i += 1) {
      if (diagnostics[i].severity === "error") {
        pairs.push({ code: diagnostics[i].code, message: diagnostics[i].message });
      }
    }
    return pairs;
  }

  function runConformanceCase(testCase) {
    var files = testCase.files || testCase.source_files || {};
    var result = MI.execute(testCase.source, { files: files });
    var expect = testCase.expect || {};
    var expectedOutput = expect.output || [];
    var expectedErrors = expect.errors || [];
    var actualErrors = errorPairs(result.diagnostics);

    var outputOk =
      result.output.length === expectedOutput.length &&
      result.output.every(function (line, index) {
        return line === expectedOutput[index];
      });
    var errorsOk = true;
    var details = [];
    if (actualErrors.length < expectedErrors.length) errorsOk = false;
    for (var i = 0; i < expectedErrors.length && errorsOk; i += 1) {
      var want = expectedErrors[i];
      var got = actualErrors[i];
      if (!got || got.code !== want.code) {
        errorsOk = false;
        details.push("expected error " + want.code + " at position " + i + ", got " + (got ? got.code : "none"));
        break;
      }
      if (want.contains && String(got.message).toLowerCase().indexOf(String(want.contains).toLowerCase()) < 0) {
        errorsOk = false;
        details.push("error " + want.code + " message lacks " + JSON.stringify(want.contains));
      }
    }
    if (!expectedErrors.length && actualErrors.length) {
      errorsOk = false;
      var first = actualErrors[0];
      details.push("unexpected " + first.code + ": " + first.message);
    }
    return {
      name: testCase.name || "(unnamed case)",
      passed: outputOk && errorsOk,
      outputOk: outputOk,
      errorsOk: errorsOk,
      expectedOutput: expectedOutput,
      actualOutput: result.output,
      expectedErrors: expectedErrors,
      actualErrors: actualErrors,
      details: details
    };
  }

  function runSelfTest() {
    var cases = Array.isArray(window.MI_CONFORMANCE) ? window.MI_CONFORMANCE : [];
    var results = [];
    var passed = 0;
    var startedAt = (window.performance && performance.now) ? performance.now() : Date.now();
    for (var i = 0; i < cases.length; i += 1) {
      var outcome;
      try {
        outcome = runConformanceCase(cases[i]);
      } catch (err) {
        outcome = {
          name: cases[i].name || "case " + i,
          passed: false,
          outputOk: false,
          errorsOk: false,
          expectedOutput: [],
          actualOutput: [],
          expectedErrors: [],
          actualErrors: [],
          details: ["threw: " + (err && err.message ? err.message : String(err))]
        };
      }
      results.push(outcome);
      if (outcome.passed) passed += 1;
    }
    var elapsed = ((window.performance && performance.now ? performance.now() : Date.now()) - startedAt) / 1000;
    var summary = $("self-test-summary");
    summary.textContent =
      passed + "/" + cases.length + " passed" +
      (cases.length - passed > 0 ? " · " + (cases.length - passed) + " failed" : "") +
      " · " + elapsed.toFixed(2) + "s";
    summary.classList.toggle("is-success", passed === cases.length && cases.length > 0);
    summary.classList.toggle("is-failure", passed !== cases.length);
    renderSelfTestResults(results);
    return { passed: passed, total: cases.length, results: results };
  }

  function renderSelfTestResults(results) {
    var container = $("self-test-results");
    container.textContent = "";
    var failures = results.filter(function (result) {
      return !result.passed;
    });
    if (!results.length) {
      container.appendChild(el("p", "muted", "No conformance cases were embedded."));
      return;
    }
    if (!failures.length) {
      container.appendChild(el("p", "selftest-ok", "Every embedded conformance case passed."));
      return;
    }
    failures.forEach(function (failure) {
      var box = el("div", "selftest-case is-failure");
      box.appendChild(el("div", "selftest-name", "FAIL " + failure.name));
      failure.details.forEach(function (detail) {
        box.appendChild(el("div", "selftest-detail", detail));
      });
      if (!failure.outputOk) {
        box.appendChild(el("div", "selftest-detail", "output: " + JSON.stringify(failure.actualOutput)));
        box.appendChild(el("div", "selftest-detail", "expected: " + JSON.stringify(failure.expectedOutput)));
      }
      if (!failure.errorsOk) {
        box.appendChild(
          el(
            "div",
            "selftest-detail",
            "errors: " +
              JSON.stringify(
                failure.actualErrors.map(function (pair) {
                  return pair.code;
                })
              ) +
              " expected: " +
              JSON.stringify(
                failure.expectedErrors.map(function (pair) {
                  return pair.code;
                })
              )
          )
        );
      }
      container.appendChild(box);
    });
  }

  function wireSelfTest() {
    $("self-test-run").addEventListener("click", function () {
      activateTab("selftest");
      runSelfTest();
    });
  }

  // ---------------------------------------------------------------- running
  function ensureUIHost() {
    if (state.uiHost) return state.uiHost;
    state.uiHost = MI.createUIHost({
      container: $("ui-area"),
      document: document,
      onError: function (err) {
        var box = el("div", "diagnostic is-error");
        box.appendChild(
          el(
            "div",
            "diagnostic-head",
            "runtime error[" + ((err && err.code) || "E0300") + "] " + ((err && err.message) || "error")
          )
        );
        $("diagnostics").appendChild(box);
        $("diag-summary").textContent = "ui button failed";
      }
    });
    return state.uiHost;
  }

  function runProgram() {
    if (state.running) return;
    state.running = true;
    var editor = $("editor");
    var source = editor.value;
    clearConsole();
    $("ui-area").textContent = "";
    var uiHost = ensureUIHost();
    uiHost.clear();
    setStatus("running…");

    var startedAt = (window.performance && performance.now) ? performance.now() : Date.now();
    var result;
    try {
      result = MI.execute(source, {
        onOutput: function (line) {
          appendConsoleLine(line);
        },
        files: exampleFilesForRun(),
        uiHost: uiHost,
        filename: "main.mi"
      });
    } catch (err) {
      result = {
        output: [],
        diagnostics: [
          {
            severity: "error",
            code: "E9999",
            message: "internal runtime failure: " + ((err && err.message) || String(err)),
            span: { start: { line: 1, col: 1, offset: 0 }, end: { line: 1, col: 2, offset: 1 } },
            hint: null
          }
        ]
      };
    }
    var elapsed = ((window.performance && performance.now ? performance.now() : Date.now()) - startedAt);
    var counts = renderDiagnostics(result.diagnostics, source);
    state.lastRunMs = elapsed;
    state.running = false;
    setStatus(
      result.output.length +
        " line(s) · " +
        counts.errors +
        " error(s) · " +
        counts.warnings +
        " warning(s) · " +
        Math.round(elapsed) +
        "ms"
    );
    if (result.output.length) activateTab("console");
    return result;
  }

  function wireRunControls() {
    $("run-btn").addEventListener("click", runProgram);
    $("clear-btn").addEventListener("click", function () {
      clearConsole();
      renderDiagnostics([], $("editor").value);
      $("ui-area").textContent = "";
      setStatus("cleared");
    });
    $("reset-btn").addEventListener("click", function () {
      setEditorSource(DEFAULT_SOURCE, true);
      $("example-select").value = "";
      runProgram();
    });
  }

  // ------------------------------------------------------------------- init
  function init() {
    wireTabs();
    wireEditor();
    wireExamples();
    wireAppBar();
    wirePalette();
    wireReference();
    wireSelfTest();
    wireRunControls();

    var saved = loadSavedSource();
    var editor = $("editor");
    editor.value = saved !== null ? saved : DEFAULT_SOURCE;
    setStatus(saved !== null ? "restored from localStorage" : "ready");

    // Keyboard shortcut works even when focus is outside the textarea.
    document.addEventListener("keydown", function (event) {
      if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
        event.preventDefault();
        runProgram();
      }
    });

    runProgram();
  }

  window.MI_PLAYGROUND = {
    run: runProgram,
    runSelfTest: runSelfTest,
    openApp: openApp,
    setSource: function (source) {
      setEditorSource(source, true);
    },
    state: state,
    exampleFiles: EXAMPLE_FILES
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
