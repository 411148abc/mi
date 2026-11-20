/* MI browser runtime -- the browser UI host.
 *
 * Classic script.  Provides MI.createUIHost(options) and registers the ui.*
 * builtins in MI.hostImpls.  ui.mount/clear/heading/text/button render into the
 * playground's UI pane; ui.button calls its `press` station with
 * assembly{event: "click", id: <number>}.
 */
(function () {
  "use strict";

  window.MI = window.MI || {};
  var MI = window.MI;
  var V = MI._internals.values;
  var NOTHING = V.NOTHING;
  var display = V.display;
  var MiValueError = V.MiValueError;
  var diag = V.diag;
  var dummySpan = V.dummySpan;

  // Small DOM-free recorder so MI.execute() works outside the playground page
  // (tests, self-test tab) without needing a document.
  function RecorderHost(options) {
    options = options || {};
    this.document = options.document || (typeof document !== "undefined" ? document : null);
    this.container = options.container || null;
    this.records = [];
    this.renderRoot = null;
    this.nextButtonId = 1;
    this.fields = {};
    this.fieldCounter = 0;
    this.onError = options.onError || null;
  }

  RecorderHost.prototype._record = function (op, payload) {
    var entry = { op: op, value: payload };
    this.records.push(entry);
    return entry;
  };

  RecorderHost.prototype._root = function () {
    if (this.renderRoot) return this.renderRoot;
    if (this.container) {
      this.renderRoot = this.container;
      return this.renderRoot;
    }
    if (this.document && this.document.body) {
      var root = this.document.createElement("div");
      root.className = "mi-ui-root mi-ui-detached";
      this.document.body.appendChild(root);
      this.renderRoot = root;
      return this.renderRoot;
    }
    return null;
  };

  RecorderHost.prototype._append = function (tag, className, textContent) {
    var root = this._root();
    if (!root || !this.document) return null;
    var el = this.document.createElement(tag);
    if (className) el.className = className;
    if (textContent !== undefined) el.textContent = textContent;
    root.appendChild(el);
    return el;
  };

  RecorderHost.prototype.mount = function (target) {
    this._record("mount", target === undefined ? null : target);
    if (target === undefined || target === null || target === "") {
      this.renderRoot = this.container || this._root();
      return this.renderRoot;
    }
    var resolved = target;
    if (typeof target === "string" && this.document) {
      resolved = this.document.querySelector(target) || this.document.getElementById(target);
    }
    if (resolved) this.renderRoot = resolved;
    return this.renderRoot;
  };

  RecorderHost.prototype.clear = function () {
    this._record("clear", null);
    this.nextButtonId = 1;
    this.fields = {};
    this.fieldCounter = 0;
    var root = this._root();
    if (root) {
      while (root.firstChild) root.removeChild(root.firstChild);
    }
  };

  RecorderHost.prototype.heading = function (value) {
    this._record("heading", value);
    var el = this._append("h3", "mi-ui-heading", value);
    return el;
  };

  RecorderHost.prototype.text = function (value) {
    this._record("text", value);
    var el = this._append("p", "mi-ui-text", value);
    return el;
  };

  RecorderHost.prototype.input = function (label, initialValue) {
    this.fieldCounter += 1;
    this._record("input", { label: label, value: initialValue });
    var root = this._root();
    var documentRef = this.document;
    if (!root || !documentRef) {
      this.fields[label] = null;
      return null;
    }
    var wrapper = documentRef.createElement("label");
    wrapper.className = "mi-ui-field";
    wrapper.setAttribute("data-mi-label", label);
    var caption = documentRef.createElement("span");
    caption.className = "mi-ui-field-label";
    caption.textContent = label;
    var input = documentRef.createElement("input");
    input.type = "text";
    input.className = "mi-ui-input";
    input.setAttribute("data-mi-label", label);
    input.value = initialValue === undefined || initialValue === null ? "" : String(initialValue);
    wrapper.appendChild(caption);
    wrapper.appendChild(input);
    root.appendChild(wrapper);
    this.fields[label] = input;
    return input;
  };

  RecorderHost.prototype.value = function (label) {
    this._record("value", label);
    if (!Object.prototype.hasOwnProperty.call(this.fields, label)) {
      throw new MiValueError("there is no input labelled `" + label + "`");
    }
    var input = this.fields[label];
    return input && typeof input.value === "string" ? input.value : "";
  };

  RecorderHost.prototype.button = function (label, onPress, id) {
    var buttonId = id === undefined ? this.nextButtonId++ : id;
    this._record("button", { label: label, id: buttonId });
    var el = this._append("button", "mi-ui-button", label);
    if (el && typeof onPress === "function") {
      el.setAttribute("type", "button");
      el.setAttribute("data-mi-button-id", String(buttonId));
      el.addEventListener("click", function (event) {
        if (event && typeof event.preventDefault === "function") event.preventDefault();
        onPress(buttonId);
      });
    }
    return el;
  };

  MI.createUIHost = function (options) {
    return new RecorderHost(options || {});
  };

  function uiHostOf(runtime, span) {
    if (!runtime.uiHost) {
      runtime.fail("E0308", "the browser UI host is not available", span);
    }
    return runtime.uiHost;
  }

  function uiMount(runtime, bound, span) {
    uiHostOf(runtime, span).mount(bound.target);
    return NOTHING;
  }

  function uiClear(runtime, bound, span) {
    uiHostOf(runtime, span).clear();
    return NOTHING;
  }

  function uiHeading(runtime, bound, span) {
    uiHostOf(runtime, span).heading(display(bound.value));
    return NOTHING;
  }

  function uiText(runtime, bound, span) {
    uiHostOf(runtime, span).text(display(bound.value));
    return NOTHING;
  }

  function uiInput(runtime, bound, span) {
    var host = uiHostOf(runtime, span);
    var label = display(bound.label);
    var initial = Object.prototype.hasOwnProperty.call(bound, "value") ? display(bound.value) : "";
    host.input(label, initial);
    return NOTHING;
  }

  function uiValue(runtime, bound, span) {
    var host = uiHostOf(runtime, span);
    var label = display(bound.label);
    try {
      return host.value(label);
    } catch (err) {
      if (err instanceof MiValueError) runtime.fail(err.code, err.message, span);
      throw err;
    }
  }

  function uiButton(runtime, bound, span) {
    var host = uiHostOf(runtime, span);
    var label = display(bound.label);
    var press = bound.press;
    var hasValue = Object.prototype.hasOwnProperty.call(bound, "value");
    var id = host.nextButtonId;
    host.nextButtonId += 1;
    var clickAssembly = V.Assembly.fromPairs([
      ["event", "click"],
      ["id", id],
      ["value", hasValue ? bound.value : NOTHING]
    ]);
    host.button(label, function () {
      try {
        runtime.callValue(press, [[null, clickAssembly]], span);
      } catch (err) {
        if (runtime.onError) {
          runtime.onError(err);
        } else if (typeof console !== "undefined" && console.error) {
          console.error("MI ui.button press failed:", err && err.message ? err.message : err);
        }
      }
    }, id);
    return NOTHING;
  }

  MI.hostImpls = {
    "ui.mount": uiMount,
    "ui.clear": uiClear,
    "ui.heading": uiHeading,
    "ui.text": uiText,
    "ui.input": uiInput,
    "ui.value": uiValue,
    "ui.button": uiButton
  };

  MI._internals.ui = { RecorderHost: RecorderHost };
})();
