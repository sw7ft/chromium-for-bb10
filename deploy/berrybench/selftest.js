// Math self-test for the Berry Browser engine. Returns a JSON string.
//
// Every check feeds its operands through an array so neither the parser nor
// the optimizer can pre-compute the answer, and runs the operation exactly
// once in a fresh closure, i.e. in cold (interpreter/baseline) code. That is
// the path that was broken before the 2026-11-20 float-ABI fix: V8's embedded
// builtins called the C math routines with the hard-float convention while
// the phone's code is softfp.
(function () {
  var v = [1, 2, 10, 5.5, 0.5, 27, 3, 4, 1000, 8, 1795176000123, -7.5];
  function close(a, b) {
    return Math.abs(a - b) <= 1e-12 * Math.max(1, Math.abs(b));
  }
  var checks = [
    ["Math.sin(1)", function () { return Math.sin(v[0]); }, 0.8414709848078965],
    ["Math.cos(1)", function () { return Math.cos(v[0]); }, 0.5403023058681398],
    ["Math.tan(1)", function () { return Math.tan(v[0]); }, 1.5574077246549023],
    ["Math.asin(0.5)", function () { return Math.asin(v[4]); }, 0.5235987755982989],
    ["Math.acos(0.5)", function () { return Math.acos(v[4]); }, 1.0471975511965979],
    ["Math.atan(1)", function () { return Math.atan(v[0]); }, 0.7853981633974483],
    ["Math.atan2(1,2)", function () { return Math.atan2(v[0], v[1]); }, 0.4636476090008061],
    ["Math.exp(1)", function () { return Math.exp(v[0]); }, 2.718281828459045],
    ["Math.expm1(1)", function () { return Math.expm1(v[0]); }, 1.718281828459045],
    ["Math.log(10)", function () { return Math.log(v[2]); }, 2.302585092994046],
    ["Math.log1p(1)", function () { return Math.log1p(v[0]); }, 0.6931471805599453],
    ["Math.log2(8)", function () { return Math.log2(v[9]); }, 3],
    ["Math.log10(1000)", function () { return Math.log10(v[8]); }, 3],
    ["Math.cbrt(27)", function () { return Math.cbrt(v[5]); }, 3],
    ["Math.sinh(1)", function () { return Math.sinh(v[0]); }, 1.1752011936438014],
    ["Math.cosh(1)", function () { return Math.cosh(v[0]); }, 1.5430806348152437],
    ["Math.tanh(1)", function () { return Math.tanh(v[0]); }, 0.7615941559557649],
    ["Math.pow(2,10)", function () { return Math.pow(v[1], v[2]); }, 1024],
    ["Math.pow(2,0.5)", function () { return Math.pow(v[1], v[4]); }, 1.4142135623730951],
    ["Math.pow(2,53)-1", function () { return Math.pow(v[1], 53) - 1; }, 9007199254740991],
    ["2 ** 10", function () { return v[1] ** v[2]; }, 1024],
    ["5.5 % 2", function () { return v[3] % v[1]; }, 1.5],
    ["-7.5 % 2", function () { return v[11] % v[1]; }, -1.5],
    ["timestamp % 1000", function () { return v[10] % v[8]; }, 123],
    ["Math.hypot(3,4)", function () { return Math.hypot(v[6], v[7]); }, 5],
    ["Math.sqrt(2)", function () { return Math.sqrt(v[1]); }, 1.4142135623730951],
  ];
  var failed = [];
  for (var i = 0; i < checks.length; i++) {
    var got;
    try { got = checks[i][1](); } catch (e) { got = "threw " + e; }
    if (typeof got !== "number" || !close(got, checks[i][2]))
      failed.push(checks[i][0] + " = " + got + " (want " + checks[i][2] + ")");
  }
  return JSON.stringify({ total: checks.length, passed: checks.length - failed.length, failed: failed });
})()
