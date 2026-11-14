// The maths functions JavaScript engines (V8) use for Math.sin, Math.pow and the rest: a port of fdlibm that gives the
// same answer on every machine, which the system maths library does not. Used so a render here matches the web app bit
// for bit. Source: V8 12.4.254.21 (the engine in Node 22), src/base/ieee754.cc, with only the V8 helper types swapped
// for stand-ins. See LICENSE-fdlibm.txt.
#pragma once

namespace wax {
namespace v8math {
double acos(double x);
double acosh(double x);
double asin(double x);
double asinh(double x);
double atan(double x);
double atan2(double y, double x);
double cos(double x);
double sin(double x);
double exp(double x);
double atanh(double x);
double log(double x);
double log1p(double x);
double log2(double x);
double log10(double x);
double cbrt(double x);
double expm1(double x);
double pow(double x, double y);
double tan(double x);
double cosh(double x);
double sinh(double x);
double tanh(double x);
}  // namespace v8math
}  // namespace wax
