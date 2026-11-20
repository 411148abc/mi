/* MI browser runtime -- builtin implementations.
 *
 * Classic script: registers MI.builtinImpls (canonical name -> impl) and the
 * host-independent `show` output port.  Ported from 米/builtins.py; the
 * browser-only ui.* builtins live in ui_host.js.
 */
(function () {
  "use strict";

  window.MI = window.MI || {};
  var MI = window.MI;
  var V = MI._internals.values;
  var NOTHING = V.NOTHING;
  var Assembly = V.Assembly;
  var MiValueError = V.MiValueError;
  var display = V.display;
  var kindOf = V.kindOf;
  var truthy = V.truthy;
  var equal = V.equal;
  var asInt = V.asInt;
  var asText = V.asText;
  var asList = V.asList;

  function pyRepr(text) {
    var hasSingle = text.indexOf("'") >= 0;
    var hasDouble = text.indexOf('"') >= 0;
    var body;
    if (hasSingle && !hasDouble) {
      body = text.replace(/\\/g, "\\\\").replace(/"/g, '\\"').replace(/\n/g, "\\n").replace(/\t/g, "\\t").replace(/\r/g, "\\r");
      return '"' + body + '"';
    }
    body = text.replace(/\\/g, "\\\\").replace(/'/g, "\\'").replace(/\n/g, "\\n").replace(/\t/g, "\\t").replace(/\r/g, "\\r");
    return "'" + body + "'";
  }

  function isNumber(value) {
    return typeof value === "number";
  }

  function show(runtime, bound) {
    var values = bound.value || [];
    var parts = [];
    for (var i = 0; i < values.length; i += 1) parts.push(display(values[i]));
    runtime.emitLine(parts.join(" "));
    return NOTHING;
  }

  function len(runtime, bound) {
    var value = bound.value;
    if (typeof value === "string" || Array.isArray(value) || value instanceof Assembly) {
      if (typeof value === "string") return value.length;
      if (Array.isArray(value)) return value.length;
      return value.fields.length;
    }
    throw new MiValueError("`len` needs text, a list or an assembly, not " + kindOf(value));
  }

  function push(runtime, bound) {
    var items = asList(bound.into, "`push`'s `into`");
    return items.concat([bound.value]);
  }

  function at(runtime, bound) {
    var items = bound.items;
    var index = asInt(bound.index, "`at`'s `index`");
    if (!(Array.isArray(items) || typeof items === "string")) {
      throw new MiValueError("`at` needs a list or text, not " + kindOf(items));
    }
    var length = items.length;
    var index2 = index < 0 ? index + length : index;
    if (index2 < 0 || index2 >= length) {
      throw new MiValueError("index " + index + " is out of range for a " + kindOf(items) + " of length " + length);
    }
    return typeof items === "string" ? items.charAt(index2) : items[index2];
  }

  function pythonSlice(items, start, stop) {
    var length = items.length;
    var s = start < 0 ? Math.max(0, length + start) : Math.min(start, length);
    var e = stop < 0 ? Math.max(0, length + stop) : Math.min(stop, length);
    if (e < s) e = s;
    return items.slice(s, e);
  }

  function slice(runtime, bound) {
    var items = bound.items;
    if (!(Array.isArray(items) || typeof items === "string")) {
      throw new MiValueError("`slice` needs a list or text, not " + kindOf(items));
    }
    var start = asInt(bound.from === undefined ? 0 : bound.from, "`slice`'s `from`");
    var end = bound.to;
    var stop = end === undefined || end === null ? items.length : asInt(end, "`slice`'s `to`");
    return pythonSlice(items, start, stop);
  }

  function map(runtime, bound, span) {
    var items = asList(bound.items, "`map`'s `items`");
    var fn = bound.with;
    var out = [];
    for (var i = 0; i < items.length; i += 1) out.push(runtime.callValue(fn, [[null, items[i]]], span));
    return out;
  }

  function filter(runtime, bound, span) {
    var items = asList(bound.items, "`filter`'s `items`");
    var fn = bound.by;
    var out = [];
    for (var i = 0; i < items.length; i += 1) {
      if (truthy(runtime.callValue(fn, [[null, items[i]]], span))) out.push(items[i]);
    }
    return out;
  }

  function fold(runtime, bound, span) {
    var items = asList(bound.items, "`fold`'s `items`");
    var fn = bound.with;
    var acc = bound.from;
    for (var i = 0; i < items.length; i += 1) acc = runtime.callValue(fn, [[null, acc], [null, items[i]]], span);
    return acc;
  }

  function span(runtime, bound) {
    var start = asInt(bound.from, "`span`'s `from`");
    var stop = asInt(bound.to, "`span`'s `to`");
    var step = asInt(bound.step === undefined ? 1 : bound.step, "`span`'s `step`");
    if (step === 0) throw new MiValueError("`span`'s `step` cannot be zero");
    var size = 0;
    if (step > 0) size = Math.max(0, Math.floor((stop - start + step - 1) / step));
    else size = Math.max(0, Math.floor((start - stop - step - 1) / -step));
    if (size > 1000000) throw new MiValueError("`span` is too large", "E0305");
    var out = [];
    if (step > 0) {
      for (var i = start; i < stop; i += step) out.push(i);
    } else {
      for (var j = start; j > stop; j += step) out.push(j);
    }
    return out;
  }

  function join(runtime, bound) {
    var items = asList(bound.items, "`join`'s `items`");
    var separator = bound.with === undefined ? "" : bound.with;
    if (typeof separator !== "string") throw new MiValueError("`join`'s separator must be text, not " + kindOf(separator));
    var parts = [];
    for (var i = 0; i < items.length; i += 1) parts.push(display(items[i]));
    return parts.join(separator);
  }

  function split(runtime, bound) {
    var text = asText(bound.text, "`split`'s `text`");
    var separator = asText(bound.at, "`split`'s separator");
    if (separator === "") throw new MiValueError("`split` needs a non-empty separator");
    return text.split(separator);
  }

  function text(runtime, bound) {
    return display(bound.value);
  }

  function number(runtime, bound) {
    var value = bound.value;
    if (typeof value === "boolean") throw new MiValueError("cannot convert a flag to a number");
    if (isNumber(value)) return value;
    if (typeof value === "string") {
      var trimmed = value.trim();
      var radix = 0;
      var signed = trimmed;
      if (/^[+-]/.test(signed)) signed = signed.slice(1);
      if (/^0[xX][0-9a-fA-F]+$/.test(signed)) radix = 16;
      else if (/^0[bB][01]+$/.test(signed)) radix = 2;
      else if (/^0[oO][0-7]+$/.test(signed)) radix = 8;
      if (radix) {
        var sign = trimmed.charAt(0) === "-" ? -1 : 1;
        var digits = signed.slice(2);
        return sign * parseInt(digits, radix);
      }
      if (/^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/.test(trimmed)) return Number(trimmed);
      if (/^[+-]?(inf|infinity)$/i.test(trimmed)) return trimmed.charAt(0) === "-" ? -Infinity : Infinity;
      if (/^[+-]?nan$/i.test(trimmed)) return NaN;
      throw new MiValueError("cannot read " + pyRepr(value) + " as a number");
    }
    throw new MiValueError("cannot convert " + kindOf(value) + " to a number");
  }

  function kind(runtime, bound) {
    return kindOf(bound.value);
  }

  function has(runtime, bound) {
    var container = bound.container;
    var value = bound.value;
    if (Array.isArray(container)) {
      for (var i = 0; i < container.length; i += 1) if (equal(container[i], value)) return true;
      return false;
    }
    if (container instanceof Assembly) return container.has(asText(value, "`has`'s `value`"));
    if (typeof container === "string") return container.indexOf(asText(value, "`has`'s `value`")) >= 0;
    throw new MiValueError("`has` needs a list, assembly or text, not " + kindOf(container));
  }

  function keys(runtime, bound) {
    var assembly = bound.target;
    if (!(assembly instanceof Assembly)) throw new MiValueError("`keys` needs an assembly, not " + kindOf(assembly));
    return assembly.keys();
  }

  function place(runtime, bound) {
    var assembly = bound.target;
    if (!(assembly instanceof Assembly)) throw new MiValueError("`place` needs an assembly, not " + kindOf(assembly));
    var field = asText(bound.field, "`place`'s `field`");
    return assembly.withField(field, bound.value);
  }

  function list(runtime, bound) {
    return (bound.value || []).slice();
  }

  function sort(runtime, bound) {
    var items = asList(bound.items, "`sort`'s `items`").slice();
    var allNumbers = items.every(function (item) {
      return isNumber(item);
    });
    var allTexts = items.every(function (item) {
      return typeof item === "string";
    });
    if (!allNumbers && !allTexts) {
      throw new MiValueError("`sort` needs items that can be compared with each other");
    }
    if (allNumbers) {
      items.sort(function (a, b) {
        return a - b;
      });
    } else {
      items.sort(function (a, b) {
        return a < b ? -1 : a > b ? 1 : 0;
      });
    }
    return items;
  }

  function reverse(runtime, bound) {
    var items = bound.items;
    if (Array.isArray(items)) return items.slice().reverse();
    if (typeof items === "string") return items.split("").reverse().join("");
    throw new MiValueError("`reverse` needs a list or text, not " + kindOf(items));
  }

  function asNumber(value, what) {
    if (!isNumber(value)) throw new MiValueError(what + " needs a number, not " + kindOf(value));
    return value;
  }

  function abs(runtime, bound) {
    return Math.abs(asNumber(bound.value, "`abs`"));
  }

  function floor(runtime, bound) {
    return Math.floor(asNumber(bound.value, "`floor`"));
  }

  function ceil(runtime, bound) {
    return Math.ceil(asNumber(bound.value, "`ceil`"));
  }

  // Python's round() uses banker's rounding (half to even).
  function roundHalfEven(x) {
    var floorValue = Math.floor(x);
    var diff = x - floorValue;
    var eps = 1e-12;
    if (Math.abs(diff - 0.5) < eps) {
      return floorValue % 2 === 0 ? floorValue : floorValue + 1;
    }
    if (Math.abs(diff + 0.5) < eps) {
      return floorValue % 2 === 0 ? floorValue : floorValue + 1;
    }
    return Math.round(x);
  }

  function round(runtime, bound) {
    var value = asNumber(bound.value, "`round`");
    var digits = bound.digits;
    if (digits === undefined || digits === null) return roundHalfEven(value);
    var places = asInt(digits, "`round`'s `digits`");
    var scale = Math.pow(10, places);
    var scaled = value * scale;
    return roundHalfEven(scaled) / scale;
  }

  function sqrt(runtime, bound) {
    var value = asNumber(bound.value, "`sqrt`");
    if (value < 0) throw new MiValueError("cannot take the square root of a negative number");
    return Math.sqrt(value);
  }

  function minmax(values, which) {
    values = values || [];
    if (values.length === 1 && Array.isArray(values[0])) values = values[0];
    if (!values.length) throw new MiValueError("`" + which + "` needs at least one value");
    var allNumbers = values.every(function (item) {
      return isNumber(item);
    });
    var allTexts = values.every(function (item) {
      return typeof item === "string";
    });
    if (!allNumbers && !allTexts) throw new MiValueError("`" + which + "` needs values that can be compared");
    var best = values[0];
    for (var i = 1; i < values.length; i += 1) {
      var item = values[i];
      if (which === "min" ? item < best : item > best) best = item;
    }
    return best;
  }

  function min(runtime, bound) {
    return minmax(bound.value, "min");
  }

  function max(runtime, bound) {
    return minmax(bound.value, "max");
  }

  function random(runtime, bound) {
    var start = bound.from === undefined ? 0 : bound.from;
    var stop = bound.to === undefined ? 1 : bound.to;
    if (!isNumber(start) || !isNumber(stop)) throw new MiValueError("`random` needs numbers");
    if (Number.isInteger(start) && Number.isInteger(stop)) {
      if (stop <= start) throw new MiValueError("`random` needs `to` greater than `from` for whole numbers");
      return start + Math.floor(Math.random() * (stop - start));
    }
    return start + Math.random() * (stop - start);
  }

  function clock() {
    return Date.now() / 1000;
  }

  MI.builtinImpls = {
    show: show,
    len: len,
    push: push,
    at: at,
    slice: slice,
    map: map,
    filter: filter,
    fold: fold,
    span: span,
    join: join,
    split: split,
    text: text,
    number: number,
    kind: kind,
    has: has,
    keys: keys,
    place: place,
    list: list,
    sort: sort,
    reverse: reverse,
    abs: abs,
    floor: floor,
    ceil: ceil,
    round: round,
    sqrt: sqrt,
    min: min,
    max: max,
    random: random,
    clock: clock
  };
  MI._internals.pyRepr = pyRepr;
})();
