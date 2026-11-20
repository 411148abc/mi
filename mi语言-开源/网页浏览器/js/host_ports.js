/* MI browser runtime -- host ports shared with the Python host.
 *
 * Classic script.  Registers the JSON <-> MI converters and the synchronous
 * HTTP ports in MI.builtinImpls:
 *
 *   json.parse(text) -> any        json.text(value) -> text
 *   net.get(url)     -> assembly{status, ok, body}
 *   net.post(url, body) -> assembly{status, ok, body}
 *
 * Non-2xx responses are not errors (`ok` is off); unreachable servers raise
 * E0309.  Ported from 米/jsonio.py + 米/宿主/python_host.py.
 */
(function () {
  "use strict";

  window.MI = window.MI || {};
  var MI = window.MI;
  var V = MI._internals.values;
  var NOTHING = V.NOTHING;
  var Assembly = V.Assembly;
  var MiValueError = V.MiValueError;
  var asText = V.asText;
  var kindOf = V.kindOf;

  // ------------------------------------------------------------- JSON <-> MI
  function fromJson(value) {
    if (value === null) return NOTHING;
    if (value === true || value === false) return value;
    if (typeof value === "number" || typeof value === "string") return value;
    if (Array.isArray(value)) {
      var items = [];
      for (var i = 0; i < value.length; i += 1) items.push(fromJson(value[i]));
      return items;
    }
    if (typeof value === "object") {
      var pairs = [];
      var keys = Object.keys(value);
      for (var j = 0; j < keys.length; j += 1) pairs.push([keys[j], fromJson(value[keys[j]])]);
      return Assembly.fromPairs(pairs);
    }
    throw new MiValueError("cannot convert this JSON value to an MI value");
  }

  function toJson(value) {
    if (value === NOTHING) return null;
    if (typeof value === "boolean") return value;
    if (typeof value === "number") return value;
    if (typeof value === "string") return value;
    if (Array.isArray(value)) {
      var items = [];
      for (var i = 0; i < value.length; i += 1) items.push(toJson(value[i]));
      return items;
    }
    if (value instanceof Assembly) {
      var out = {};
      for (var j = 0; j < value.fields.length; j += 1) {
        out[value.fields[j][0]] = toJson(value.fields[j][1]);
      }
      return out;
    }
    throw new MiValueError(
      "cannot write " + kindOf(value) + " to JSON; only number, text, flag, nothing, list and assembly"
    );
  }

  function jsonParse(runtime, bound) {
    var text = asText(bound.text, "`json.parse`'s `text`");
    var parsed;
    try {
      parsed = JSON.parse(text);
    } catch (err) {
      throw new MiValueError("invalid JSON: " + (err && err.message ? err.message : String(err)));
    }
    return fromJson(parsed);
  }

  function jsonText(runtime, bound) {
    var encoded;
    try {
      encoded = JSON.stringify(toJson(bound.value));
    } catch (err) {
      if (err instanceof MiValueError) throw err;
      throw new MiValueError("cannot serialise this value as JSON: " + (err && err.message ? err.message : String(err)));
    }
    if (encoded === undefined) {
      throw new MiValueError("cannot serialise this value as JSON");
    }
    return encoded;
  }

  // -------------------------------------------------------------------- net
  function networkError(url, detail) {
    var suffix = detail ? ": " + detail : "";
    return new MiValueError("network error while contacting `" + url + "`" + suffix, "E0309");
  }

  function request(method, url, body) {
    var xhr = new XMLHttpRequest();
    try {
      xhr.open(method, url, false);
    } catch (err) {
      throw networkError(url, err && err.message ? err.message : String(err));
    }
    try {
      if (method === "POST") {
        xhr.setRequestHeader("Content-Type", "text/plain; charset=utf-8");
      }
      xhr.send(body === undefined ? null : body);
    } catch (err) {
      throw networkError(url, err && err.message ? err.message : String(err));
    }
    if (xhr.status === 0) {
      // Network failure / CORS rejection: status is 0 and no response headers.
      throw networkError(url, "the server could not be reached");
    }
    var ok = xhr.status >= 200 && xhr.status < 300;
    return Assembly.fromPairs([
      ["status", xhr.status],
      ["ok", ok],
      ["body", xhr.responseText === undefined ? "" : xhr.responseText]
    ]);
  }

  function netGet(runtime, bound) {
    var url = asText(bound.url, "`net.get`'s `url`");
    return request("GET", url, undefined);
  }

  function netPost(runtime, bound) {
    var url = asText(bound.url, "`net.post`'s `url`");
    var body = bound.body;
    if (typeof body !== "string") {
      throw new MiValueError("`net.post`'s `body` must be text, not " + kindOf(body));
    }
    return request("POST", url, body);
  }

  MI.builtinImpls = MI.builtinImpls || {};
  MI.builtinImpls["json.parse"] = jsonParse;
  MI.builtinImpls["json.text"] = jsonText;
  MI.builtinImpls["net.get"] = netGet;
  MI.builtinImpls["net.post"] = netPost;

  MI._internals.jsonio = { fromJson: fromJson, toJson: toJson };
})();
