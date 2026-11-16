// genexpr: a node whose sound is code. Port of webtracker src/audio/nodes/genexpr.js and src/genexpr/.
// The web app compiles the GenExpr text to JavaScript; here the same text is parsed and run as a tree, with every
// operator evaluated in the order and with the arithmetic of the generated JavaScript, so the results match bit for bit.
// A program with any mistake produces nothing, and then there is silence (the web app also keeps the last good program,
// but a native node only ever gets its code from the node's data).
#include "nodes/all_nodes.h"
#include "nodes/genexpr_check.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/js_math.h"

namespace wax {
namespace {

constexpr int kMaxParams = 8;
constexpr double kMaxBuffer = 4194304.0;  // 1 << 22
const char* const kDefaultCode =
    "// GenExpr: in1 and in2 come in, out1 and out2 go out.\n"
    "// (To play it from notes: freq, gate, vel, note and trig; see the templates.)\n"
    "// A Param becomes a knob, p1 to p8 in the order they are declared.\n"
    "Param amount(0.5, min=0, max=1);\n"
    "History smooth(0);\n"
    "\n"
    "smooth = smooth + (in1 - smooth) * 0.1;\n"
    "out1 = mix(in1, smooth, amount);\n"
    "out2 = in2;\n";

// Where a mistake is, for the code panel (1-based line and column, and the column after its last character).
struct Where { int line = 1, col = 1, end = 0; };
struct CompileError { std::string message; Where at; };
thread_local Where gWhere;   // the place the compiler is working on: a mistake is reported here
[[noreturn]] void fail(const char* message = "this is not valid GenExpr here") { throw CompileError{message, gWhere}; }

// ---------------------------------------------------------------------------------------------------------- lexer
struct Tok {
  enum T { Num, Id, Op, Eof } t = Eof;
  std::string v;
  double num = 0;
  int line = 1, col = 1;   // where it starts
};

const char* const kOps[] = {"**", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "+", "-", "*", "/", "%",
                            "<",  ">",  "=",  "!",  "?",  ":",  ";",  ",",  "(",  ")",  "{",  "}", "."};

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }

std::vector<Tok> lex(const std::string& text) {
  std::vector<Tok> toks;
  size_t i = 0, lineStart = 0;
  int line = 1;
  const size_t len = text.size();
  const auto here = [&]() { return Where{line, static_cast<int>(i - lineStart) + 1, static_cast<int>(i - lineStart) + 2}; };
  while (i < len) {
    const char c = text[i];
    if (c == '\n') { i++; line++; lineStart = i; continue; }
    if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
    if (c == '/' && i + 1 < len && text[i + 1] == '/') { while (i < len && text[i] != '\n') i++; continue; }
    if (c == '/' && i + 1 < len && text[i + 1] == '*') {
      const size_t end = text.find("*/", i + 2);
      if (end == std::string::npos) { gWhere = here(); fail("this comment never ends (it needs */)"); }
      for (size_t q = i; q < end; q++) if (text[q] == '\n') { line++; lineStart = q + 1; }
      i = end + 2;
      continue;
    }
    const std::string w = text.substr(i, 40);  // the web lexer only looks 40 characters ahead for a number
    size_t j = 0;
    bool isNum = false;
    if (isDigit(w[0])) {
      isNum = true;
      while (j < w.size() && isDigit(w[j])) j++;
      if (j < w.size() && w[j] == '.') j++;
      while (j < w.size() && isDigit(w[j])) j++;
    } else if (w[0] == '.' && w.size() > 1 && isDigit(w[1])) {
      isNum = true;
      j = 1;
      while (j < w.size() && isDigit(w[j])) j++;
    }
    if (isNum) {
      if (j < w.size() && (w[j] == 'e' || w[j] == 'E')) {
        size_t k = j + 1;
        if (k < w.size() && (w[k] == '+' || w[k] == '-')) k++;
        if (k < w.size() && isDigit(w[k])) {
          while (k < w.size() && isDigit(w[k])) k++;
          j = k;
        }
      }
      Tok t;
      t.t = Tok::Num;
      t.v = w.substr(0, j);
      t.num = std::strtod(t.v.c_str(), nullptr);
      t.line = line; t.col = static_cast<int>(i - lineStart) + 1;
      toks.push_back(t);
      i += j;
    } else if (isIdStart(c)) {
      const std::string w2 = text.substr(i, 80);
      size_t k = 0;
      while (k < w2.size() && (isIdStart(w2[k]) || isDigit(w2[k]))) k++;
      Tok t;
      t.t = Tok::Id;
      t.v = w2.substr(0, k);
      t.line = line; t.col = static_cast<int>(i - lineStart) + 1;
      toks.push_back(t);
      i += k;
    } else {
      bool found = false;
      for (const char* op : kOps) {
        const size_t n = std::char_traits<char>::length(op);
        if (text.compare(i, n, op) == 0) {
          Tok t;
          t.t = Tok::Op;
          t.v = op;
          t.line = line; t.col = static_cast<int>(i - lineStart) + 1;
          toks.push_back(t);
          i += n;
          found = true;
          break;
        }
      }
      if (!found) { gWhere = here(); fail("this character is not part of GenExpr"); }
    }
  }
  Tok eof;
  eof.line = line; eof.col = static_cast<int>(len - lineStart) + 1;
  toks.push_back(eof);
  return toks;
}

// -------------------------------------------------------------------------------------------------------- parser
struct PNode;
using P = std::shared_ptr<PNode>;
struct PNode {
  enum K { Num, Id, Call, Method, Un, Bin, Cond } k = Num;
  double num = 0;
  std::string name, op;
  std::vector<P> args;  // call and method arguments
  P l, r, c;            // bin: l r; un: l; cond: c l r; method: l is the object
  Where at;             // where it starts
};

struct PStmt;
using S = std::shared_ptr<PStmt>;
struct DeclArg { bool keyed = false; std::string key; P value; };
struct PStmt {
  enum K { If, Decl, Tuple, Set, Expr } k = Expr;
  P cond;                        // if
  std::vector<S> then, other;    // if
  bool hasOther = false;
  std::string kind, name, op;    // decl: kind name; set: name op
  std::vector<DeclArg> args;     // decl
  std::vector<std::string> names;  // tuple
  std::vector<P> values;         // tuple; set uses values[0]
  P e;                           // expr
  Where at;                      // where it starts
};

class Parser {
public:
  explicit Parser(std::vector<Tok> t) : toks_(std::move(t)) {}

  // A mistake at the token about to be read.
  [[noreturn]] void fail(const char* message) const {
    gWhere = {peek().line, peek().col, peek().col + static_cast<int>(std::max<size_t>(1, peek().v.size()))};
    ::wax::fail(message);
  }

  std::vector<S> parse() {
    std::vector<S> body;
    while (peek().t != Tok::Eof) {
      if (is("}")) fail("this } closes nothing");
      body.push_back(statement());
    }
    return body;
  }

private:
  std::vector<Tok> toks_;
  size_t p_ = 0;

  const Tok& at(size_t i) const { return i < toks_.size() ? toks_[i] : toks_.back(); }
  const Tok& peek() const { return at(p_); }
  const Tok& next() { const Tok& t = at(p_); p_++; return t; }
  bool is(const char* v, Tok::T t = Tok::Op) const { return peek().t == t && peek().v == v; }
  bool isAt(size_t i, const char* v) const { return at(i).t == Tok::Op && at(i).v == v; }
  void expect(const char* v) {
    if (!is(v)) { const std::string m = std::string("expected ") + v + (peek().t == Tok::Eof ? " but the code ends here" : " here"); fail(m.c_str()); }
    next();
  }

  P mk(PNode::K k) {
    auto n = std::make_shared<PNode>();
    n->k = k;
    const Tok& t = at(p_ > 0 ? p_ - 1 : 0);   // (the token just read)
    n->at = {t.line, t.col, t.col + static_cast<int>(std::max<size_t>(1, t.v.size()))};
    return n;
  }

  void arglist(std::vector<P>& args) {
    if (!is(")")) {
      for (;;) {
        args.push_back(expr());
        if (is(",")) next(); else break;
      }
    }
  }

  P primary() {
    const Tok tk = next();
    if (tk.t == Tok::Num) { P n = mk(PNode::Num); n->num = tk.num; return n; }
    if (tk.t == Tok::Id) {
      if (is("(")) {
        next();
        P n = mk(PNode::Call);
        n->name = tk.v;
        arglist(n->args);
        expect(")");
        return n;
      }
      P n = mk(PNode::Id);
      n->name = tk.v;
      return n;
    }
    if (tk.t == Tok::Op && tk.v == "(") { P e = expr(); expect(")"); return e; }
    gWhere = {tk.line, tk.col, tk.col + static_cast<int>(std::max<size_t>(1, tk.v.size()))};
    ::wax::fail(tk.t == Tok::Eof ? "the code ends where a value was expected" : "a value was expected here");
  }

  P postfix() {
    P e = primary();
    while (is(".")) {
      next();
      if (peek().t != Tok::Id) fail("a method name should follow the dot");
      const std::string name = next().v;
      expect("(");
      P n = mk(PNode::Method);
      n->name = name;
      n->l = e;
      arglist(n->args);
      expect(")");
      e = n;
    }
    return e;
  }

  P unary() {
    if (is("-") || is("!") || is("+")) {
      const std::string op = next().v;
      P e = unary();
      if (op == "+") return e;
      P n = mk(PNode::Un);
      n->op = op;
      n->l = e;
      return n;
    }
    return power();
  }

  P power() {
    P l = postfix();
    if (is("**")) {
      next();
      P r = unary();
      P n = mk(PNode::Bin);
      n->op = "**"; n->l = l; n->r = r;
      return n;
    }
    return l;
  }

  template <size_t N>
  P level(const char* const (&ops)[N], P (Parser::*sub)()) {
    P l = (this->*sub)();
    for (;;) {
      bool hit = false;
      if (peek().t == Tok::Op) for (const char* o : ops) if (peek().v == o) hit = true;
      if (!hit) break;
      const std::string op = next().v;
      P r = (this->*sub)();
      P n = mk(PNode::Bin);
      n->op = op; n->l = l; n->r = r;
      l = n;
    }
    return l;
  }
  P mulL() { static const char* const ops[] = {"*", "/", "%"}; return level(ops, &Parser::unary); }
  P addL() { static const char* const ops[] = {"+", "-"}; return level(ops, &Parser::mulL); }
  P relL() { static const char* const ops[] = {"<", ">", "<=", ">="}; return level(ops, &Parser::addL); }
  P eqL() { static const char* const ops[] = {"==", "!="}; return level(ops, &Parser::relL); }
  P andL() { static const char* const ops[] = {"&&"}; return level(ops, &Parser::eqL); }
  P orL() { static const char* const ops[] = {"||"}; return level(ops, &Parser::andL); }

  P expr() {
    P c = orL();
    if (is("?")) {
      next();
      P a = expr();
      expect(":");
      P b = expr();
      P n = mk(PNode::Cond);
      n->c = c; n->l = a; n->r = b;
      return n;
    }
    return c;
  }

  std::vector<S> block() {
    expect("{");
    std::vector<S> body;
    while (!is("}") && peek().t != Tok::Eof) body.push_back(statement());
    expect("}");
    return body;
  }

  S statement() {
    auto s = std::make_shared<PStmt>();
    const Tok tk = peek();
    s->at = {tk.line, tk.col, tk.col + static_cast<int>(std::max<size_t>(1, tk.v.size()))};
    if (tk.t == Tok::Id && tk.v == "if") {
      next();
      expect("(");
      s->k = PStmt::If;
      s->cond = expr();
      expect(")");
      if (is("{")) s->then = block(); else s->then = {statement()};
      if (is("else", Tok::Id)) {
        next();
        s->hasOther = true;
        if (is("{")) s->other = block(); else s->other = {statement()};
      }
      return s;
    }
    if (tk.t == Tok::Id && (tk.v == "History" || tk.v == "Param" || tk.v == "Data" || tk.v == "Delay") && at(p_ + 1).t == Tok::Id) {
      next();
      s->k = PStmt::Decl;
      s->kind = tk.v;
      s->name = next().v;
      expect("(");
      if (!is(")")) {
        for (;;) {
          DeclArg a;
          if (peek().t == Tok::Id && isAt(p_ + 1, "=")) {
            a.keyed = true;
            a.key = next().v;
            next();
          }
          a.value = expr();
          s->args.push_back(a);
          if (is(",")) next(); else break;
        }
      }
      expect(")");
      expect(";");
      return s;
    }
    // a, b, c = x, y, z;  every value is worked out first, then the names are set
    if (tk.t == Tok::Id && isAt(p_ + 1, ",")) {
      size_t q = p_;
      int names = 0;
      while (at(q).t == Tok::Id && isAt(q + 1, ",")) { q += 2; names++; }
      if (at(q).t == Tok::Id && isAt(q + 1, "=")) {
        s->k = PStmt::Tuple;
        for (int i = 0; i <= names; i++) {
          s->names.push_back(next().v);
          if (i < names) next();
        }
        next();
        s->values.push_back(expr());
        while (is(",")) { next(); s->values.push_back(expr()); }
        expect(";");
        if (s->values.size() != s->names.size()) fail("there are not as many values as names on the left");
        return s;
      }
    }
    if (tk.t == Tok::Id && (isAt(p_ + 1, "=") || isAt(p_ + 1, "+=") || isAt(p_ + 1, "-=") || isAt(p_ + 1, "*=") || isAt(p_ + 1, "/="))) {
      next();
      s->k = PStmt::Set;
      s->name = tk.v;
      s->op = next().v;
      s->values.push_back(expr());
      expect(";");
      return s;
    }
    P e = expr();
    if (e->k != PNode::Call && e->k != PNode::Method) { gWhere = e->at; ::wax::fail("this does nothing: only a call can stand alone as a statement"); }
    expect(";");
    s->k = PStmt::Expr;
    s->e = e;
    return s;
  }
};

// -------------------------------------------------------------------------------------------------- executable tree
enum class Fn {
  Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Sinh, Cosh, Tanh, Abs, Sign, Floor, Ceil, Round, Trunc, Fract, Sqrt, Exp, Log, Log2,
  Log10, Pow, Min, Max, Clamp, Mix, Scale, Wrap, Fold, Mod, Mtof, Ftom, Dbtoa, Atodb, Mstosamps, Sampstoms, Fixdenorm, Noise,
  Phasor, Cycle, Delta, Change, Slide, Dcblock
};

struct FuncInfo { const char* name; int lo, hi; Fn fn; int state; };
const FuncInfo kFuncs[] = {
    {"sin", 1, 1, Fn::Sin, 0}, {"cos", 1, 1, Fn::Cos, 0}, {"tan", 1, 1, Fn::Tan, 0}, {"asin", 1, 1, Fn::Asin, 0},
    {"acos", 1, 1, Fn::Acos, 0}, {"atan", 1, 1, Fn::Atan, 0}, {"atan2", 2, 2, Fn::Atan2, 0}, {"sinh", 1, 1, Fn::Sinh, 0},
    {"cosh", 1, 1, Fn::Cosh, 0}, {"tanh", 1, 1, Fn::Tanh, 0}, {"abs", 1, 1, Fn::Abs, 0}, {"sign", 1, 1, Fn::Sign, 0},
    {"floor", 1, 1, Fn::Floor, 0}, {"ceil", 1, 1, Fn::Ceil, 0}, {"round", 1, 1, Fn::Round, 0}, {"trunc", 1, 1, Fn::Trunc, 0},
    {"fract", 1, 1, Fn::Fract, 0}, {"sqrt", 1, 1, Fn::Sqrt, 0}, {"exp", 1, 1, Fn::Exp, 0}, {"log", 1, 1, Fn::Log, 0},
    {"log2", 1, 1, Fn::Log2, 0}, {"log10", 1, 1, Fn::Log10, 0}, {"pow", 2, 2, Fn::Pow, 0}, {"min", 2, 2, Fn::Min, 0},
    {"max", 2, 2, Fn::Max, 0}, {"clamp", 3, 3, Fn::Clamp, 0}, {"clip", 3, 3, Fn::Clamp, 0}, {"mix", 3, 3, Fn::Mix, 0},
    {"scale", 5, 5, Fn::Scale, 0}, {"wrap", 3, 3, Fn::Wrap, 0}, {"fold", 3, 3, Fn::Fold, 0}, {"mod", 2, 2, Fn::Mod, 0},
    {"mtof", 1, 1, Fn::Mtof, 0}, {"ftom", 1, 1, Fn::Ftom, 0}, {"dbtoa", 1, 1, Fn::Dbtoa, 0}, {"atodb", 1, 1, Fn::Atodb, 0},
    {"mstosamps", 1, 1, Fn::Mstosamps, 0}, {"sampstoms", 1, 1, Fn::Sampstoms, 0}, {"fixdenorm", 1, 1, Fn::Fixdenorm, 0},
    {"noise", 0, 0, Fn::Noise, 1}, {"phasor", 1, 1, Fn::Phasor, 1}, {"cycle", 1, 1, Fn::Cycle, 1}, {"delta", 1, 1, Fn::Delta, 1},
    {"change", 1, 1, Fn::Change, 1}, {"slide", 3, 3, Fn::Slide, 1}, {"dcblock", 1, 1, Fn::Dcblock, 2},
};
const FuncInfo* findFunc(const std::string& name) {
  for (const auto& f : kFuncs) if (name == f.name) return &f;
  return nullptr;
}
bool isBufferFunc(const std::string& n) { return n == "peek" || n == "poke" || n == "dim"; }
bool isWord(const std::string& n) { return n == "if" || n == "else" || n == "History" || n == "Param" || n == "Data" || n == "Delay"; }

struct E;
using EP = std::unique_ptr<E>;
struct E {
  enum T {
    Num, In, Note, Trig, Out, Param, Hist, Local,
    Neg, Not, Div, Mod, Pow, Lt, Gt, Le, Ge, Eq, Ne, And, Or, Add, Sub, Mul, RawDiv, Cond,
    Func, Peek, Poke, PokeVoid, DelayRead, DelayWrite
  } t = Num;
  double num = 0;
  int idx = 0;   // in/note/out/param/hist/local index, state slot, buffer number
  int w = 0;     // delay write-position slot
  Fn fn = Fn::Sin;
  std::vector<EP> k;
};

struct Target { enum T { Out, Hist, Local } t = Out; int idx = 0; };
struct Stmt;
using StmtP = std::unique_ptr<Stmt>;
struct Stmt {
  enum T { Set, Tuple, If, Expr } t = Set;
  Target target;
  EP value;
  std::vector<Target> targets;  // tuple
  std::vector<EP> values;       // tuple
  EP cond;
  std::vector<StmtP> then, other;
};

struct Program {
  std::vector<StmtP> code;
  int nState = 0;
  int nLocals = 0;
  std::vector<std::pair<int, double>> inits;
  std::vector<size_t> buffers;
  struct Par { double def, min, max; };
  std::vector<std::string> paramNames;   // for the code panel
  std::vector<Par> params;
};

constexpr double kPi = 3.141592653589793;

class Compiler {
public:
  Compiler(double sampleRate) : sr_(sampleRate) {
    syms_["in1"] = {In, 0}; syms_["in2"] = {In, 1};
    syms_["out1"] = {OutS, 0}; syms_["out2"] = {OutS, 1};
    const char* notes[] = {"freq", "gate", "vel", "note", "trig"};
    for (int i = 0; i < 5; i++) syms_[notes[i]] = {NoteS, i};
    auto c = [&](const char* n, double v) { Sym s{Const, 0}; s.v = v; syms_[n] = s; };
    c("pi", kPi); c("twopi", kPi * 2); c("halfpi", kPi / 2); c("invpi", 1 / kPi); c("e", 2.718281828459045);
    c("ln2", 0.6931471805599453); c("ln10", 2.302585092994046); c("sqrt2", 1.4142135623730951); c("sqrt1_2", 0.7071067811865476);
    c("samplerate", sr_);
  }

  std::unique_ptr<Program> compile(const std::string& text) {
    Parser parser(lex(text));
    body_ = parser.parse();
    scan(body_);
    prog_ = std::make_unique<Program>();
    prog_->code = stmts(body_, 0);
    prog_->nState = nState_;
    prog_->nLocals = static_cast<int>(locals_.size());
    return std::move(prog_);
  }

private:
  enum Kind { In, OutS, NoteS, Const, History, ParamS, DataS, DelayS };
  struct Sym { Kind kind; int n; double v = 0; int b = 0, w = -1; double size = 0; };
  double sr_;
  std::map<std::string, Sym> syms_;
  std::vector<S> body_;
  std::unique_ptr<Program> prog_;
  std::map<std::string, int> locals_;
  std::map<std::string, bool> assigned_, setNames_;
  int nState_ = 0;

  int state(int n) { const int k = nState_; nState_ += n; return k; }

  void scan(const std::vector<S>& list) {
    for (const auto& s : list) {
      if (s->k == PStmt::Set) setNames_[s->name] = true;
      else if (s->k == PStmt::Tuple) for (const auto& n : s->names) setNames_[n] = true;
      else if (s->k == PStmt::If) { scan(s->then); if (s->hasOther) scan(s->other); }
    }
  }

  static EP mk(E::T t) { auto e = std::make_unique<E>(); e->t = t; return e; }
  static EP num(double v) { auto e = mk(E::Num); e->num = v; return e; }

  // constants for declarations
  double constant(const P& e) {
    if (e->k == PNode::Num) return e->num;
    if (e->k == PNode::Id) { auto it = syms_.find(e->name); if (it != syms_.end() && it->second.kind == Const) return it->second.v; }
    if (e->k == PNode::Un && e->op == "-") return -constant(e->l);
    if (e->k == PNode::Bin && (e->op == "+" || e->op == "-" || e->op == "*" || e->op == "/")) {
      const double a = constant(e->l);
      const double b = constant(e->r);
      return e->op == "+" ? a + b : e->op == "-" ? a - b : e->op == "*" ? a * b : (b == 0 ? 0 : a / b);
    }
    fail("this has to be a plain number (a number, a constant, or sums of them)");
  }

  void declare(const PStmt& s) {
    gWhere = s.at;
    if (syms_.count(s.name) || findFunc(s.name) || isBufferFunc(s.name) || isWord(s.name)) fail("this name is already taken");
    std::vector<const DeclArg*> pos;
    const DeclArg* named[2] = {nullptr, nullptr};  // min, max
    for (const auto& a : s.args) {
      if (!a.keyed) { pos.push_back(&a); continue; }
      if (s.kind == "Param" && a.key == "min") named[0] = &a;
      else if (s.kind == "Param" && a.key == "max") named[1] = &a;
      else fail("only min and max can be named here");
    }
    if (s.kind == "History") {
      if (pos.size() > 1) fail("a History takes one starting value");
      const int k = state(1);
      prog_->inits.push_back({k, pos.empty() ? 0.0 : constant(pos[0]->value)});
      Sym y{History, k};
      syms_[s.name] = y;
    } else if (s.kind == "Param") {
      if (pos.empty()) fail("a Param needs a starting value");
      if (pos.size() > 3) fail("a Param takes a start, a min and a max");
      const double def = constant(pos[0]->value);
      const double min = named[0] ? constant(named[0]->value) : pos.size() > 1 ? constant(pos[1]->value) : std::min(0.0, def);
      const double max = named[1] ? constant(named[1]->value) : pos.size() > 2 ? constant(pos[2]->value) : std::max(1.0, def);
      if (!(min < max)) fail("min has to be below max");
      if (def < min || def > max) fail("the starting value is outside min and max");
      if (static_cast<int>(prog_->params.size()) >= kMaxParams) fail("a node has at most 8 Params");
      const int idx = static_cast<int>(prog_->params.size());
      prog_->params.push_back({def, min, max});
      prog_->paramNames.push_back(s.name);
      syms_[s.name] = Sym{ParamS, idx};
    } else {  // Data and Delay
      if (pos.size() != 1) fail("this takes one value: its size");
      const double size = std::floor(constant(pos[0]->value));
      if (!(size >= 1) || size > kMaxBuffer) fail("the size has to be from 1 to 4194304");
      Sym y{s.kind == "Data" ? DataS : DelayS, 0};
      y.b = static_cast<int>(prog_->buffers.size());
      prog_->buffers.push_back(static_cast<size_t>(size));
      y.w = s.kind == "Delay" ? state(1) : -1;
      y.size = size;
      syms_[s.name] = y;
    }
  }

  EP genId(const std::string& name) {
    auto it = syms_.find(name);
    if (it != syms_.end()) {
      const Sym& s = it->second;
      switch (s.kind) {
        case Const: return num(s.v);
        case In: { auto e = mk(E::In); e->idx = s.n; return e; }
        case NoteS: {
          if (s.n == 4) return mk(E::Trig);
          auto e = mk(E::Note); e->idx = s.n; return e;
        }
        case OutS: { auto e = mk(E::Out); e->idx = s.n; return e; }
        case ParamS: { auto e = mk(E::Param); e->idx = s.n; return e; }
        case History: { auto e = mk(E::Hist); e->idx = s.n; return e; }
        default: fail("a buffer is not a number (use peek, poke or .read)");
      }
    }
    if (findFunc(name) || isBufferFunc(name)) fail("this is a function: it needs brackets and arguments");
    if (setNames_.count(name)) {
      if (!assigned_.count(name)) fail("this is used before it is set");
      auto e = mk(E::Local); e->idx = locals_.at(name); return e;
    }
    fail("this name is not known (declare it, or set it before use)");
  }

  const Sym& bufferSym(const P& a) {
    if (a->k == PNode::Id) {
      auto it = syms_.find(a->name);
      if (it != syms_.end() && (it->second.kind == DataS || it->second.kind == DelayS)) return it->second;
    }
    fail("a Data or Delay buffer is needed here");
  }

  EP gen(const P& e) {
    gWhere = e->at;
    switch (e->k) {
      case PNode::Num: return num(std::isfinite(e->num) ? e->num : 0);
      case PNode::Id: return genId(e->name);
      case PNode::Un: {
        auto x = gen(e->l);
        auto n = mk(e->op == "-" ? E::Neg : E::Not);
        n->k.push_back(std::move(x));
        return n;
      }
      case PNode::Bin: {
        auto l = gen(e->l);
        auto r = gen(e->r);
        const std::string& o = e->op;
        E::T t = o == "/" ? E::Div : o == "%" ? E::Mod : o == "**" ? E::Pow : o == "==" ? E::Eq : o == "!=" ? E::Ne : o == "<" ? E::Lt
               : o == ">" ? E::Gt : o == "<=" ? E::Le : o == ">=" ? E::Ge : o == "&&" ? E::And : o == "||" ? E::Or
               : o == "+" ? E::Add : o == "-" ? E::Sub : E::Mul;
        auto n = mk(t);
        n->k.push_back(std::move(l));
        n->k.push_back(std::move(r));
        return n;
      }
      case PNode::Cond: {
        auto c = gen(e->c);
        auto a = gen(e->l);
        auto b = gen(e->r);
        auto n = mk(E::Cond);
        n->k.push_back(std::move(c)); n->k.push_back(std::move(a)); n->k.push_back(std::move(b));
        return n;
      }
      case PNode::Call: {
        if (isBufferFunc(e->name)) {
          const size_t want = e->name == "peek" ? 2 : e->name == "poke" ? 3 : 1;
          if (e->args.size() != want) fail(e->name == "peek" ? "peek takes a buffer and an index" : e->name == "poke" ? "poke takes a buffer, a value and an index" : "dim takes a buffer");
          const Sym& b = bufferSym(e->args[0]);
          std::vector<EP> rest;
          for (size_t i = 1; i < e->args.size(); i++) rest.push_back(gen(e->args[i]));
          if (e->name == "dim") return num(b.size);
          if (b.kind != DataS) fail("peek and poke work on a Data buffer");
          auto n = mk(e->name == "peek" ? E::Peek : E::Poke);
          n->idx = b.b;
          n->k = std::move(rest);
          return n;
        }
        const FuncInfo* f = findFunc(e->name);
        if (!f) fail("there is no function with this name");
        const int count = static_cast<int>(e->args.size());
        if (count < f->lo || count > f->hi) fail("this function does not take this many arguments");
        auto n = mk(E::Func);
        n->fn = f->fn;
        for (const auto& a : e->args) n->k.push_back(gen(a));
        if (f->state) n->idx = state(f->state);
        return n;
      }
      case PNode::Method: {
        const Sym* b = nullptr;
        if (e->l->k == PNode::Id) { auto it = syms_.find(e->l->name); if (it != syms_.end()) b = &it->second; }
        if (!b || (b->kind != DataS && b->kind != DelayS)) fail("only a Data or Delay buffer has methods");
        std::vector<EP> args;
        for (const auto& a : e->args) args.push_back(gen(a));
        if (b->kind == DelayS) {
          if (e->name == "read" && args.size() == 1) {
            auto n = mk(E::DelayRead); n->idx = b->b; n->w = b->w; n->k = std::move(args); return n;
          }
          if (e->name == "write" && args.size() == 1) {
            auto n = mk(E::DelayWrite); n->idx = b->b; n->w = b->w; n->k = std::move(args); return n;
          }
        } else {
          if (e->name == "read" && args.size() == 1) { auto n = mk(E::Peek); n->idx = b->b; n->k = std::move(args); return n; }
          if (e->name == "write" && args.size() == 2) { auto n = mk(E::PokeVoid); n->idx = b->b; n->k = std::move(args); return n; }
        }
        fail("this method needs different arguments (read takes 1; write takes 1 on a Delay, 2 on a Data)");
      }
    }
    fail();
  }

  Target target(const std::string& name) {
    auto it = syms_.find(name);
    if (it != syms_.end()) {
      const Sym& s = it->second;
      if (s.kind == OutS) return Target{Target::Out, s.n};
      if (s.kind == History) return Target{Target::Hist, s.n};
      fail("an input, a constant, a Param or a buffer cannot be set");
    }
    if (findFunc(name) || isBufferFunc(name) || isWord(name)) fail("this name belongs to GenExpr and cannot be set");
    assigned_[name] = true;
    auto l = locals_.find(name);
    if (l == locals_.end()) l = locals_.emplace(name, static_cast<int>(locals_.size())).first;
    return Target{Target::Local, l->second};
  }

  std::vector<StmtP> stmts(const std::vector<S>& list, int depth) {
    std::vector<StmtP> out;
    for (const auto& s : list) {
      gWhere = s->at;
      if (s->k == PStmt::Decl) {
        if (depth) fail("a declaration cannot be inside an if");
        declare(*s);
      } else if (s->k == PStmt::Set) {
        auto st = std::make_unique<Stmt>();
        st->t = Stmt::Set;
        EP prior;
        if (s->op != "=") prior = genId(s->name);
        EP v = gen(s->values[0]);
        if (prior) {
          E::T t = s->op[0] == '+' ? E::Add : s->op[0] == '-' ? E::Sub : s->op[0] == '*' ? E::Mul : E::RawDiv;
          auto n = mk(t);
          n->k.push_back(std::move(prior));
          n->k.push_back(std::move(v));
          v = std::move(n);
        }
        st->target = target(s->name);
        st->value = std::move(v);
        out.push_back(std::move(st));
      } else if (s->k == PStmt::Tuple) {
        auto st = std::make_unique<Stmt>();
        st->t = Stmt::Tuple;
        for (const auto& v : s->values) st->values.push_back(gen(v));
        for (const auto& n : s->names) st->targets.push_back(target(n));
        out.push_back(std::move(st));
      } else if (s->k == PStmt::If) {
        auto st = std::make_unique<Stmt>();
        st->t = Stmt::If;
        st->cond = gen(s->cond);
        st->then = stmts(s->then, depth + 1);
        if (s->hasOther) st->other = stmts(s->other, depth + 1);
        out.push_back(std::move(st));
      } else if (s->k == PStmt::Expr) {
        auto st = std::make_unique<Stmt>();
        st->t = Stmt::Expr;
        st->value = gen(s->e);
        out.push_back(std::move(st));
      }
    }
    return out;
  }
};

// ----------------------------------------------------------------------------------------------- JS number helpers
inline double fin(double x) { return (x == x && x < 1e18 && x > -1e18) ? x : 0; }
inline double jsClamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
inline double jsMin(double a, double b) {
  if (a != a || b != b) return std::nan("");
  if (a == 0 && b == 0) return std::signbit(a) ? a : b;
  return a < b ? a : b;
}
inline double jsMax(double a, double b) {
  if (a != a || b != b) return std::nan("");
  if (a == 0 && b == 0) return std::signbit(a) ? b : a;
  return a > b ? a : b;
}
inline double jsRoundX(double x) { const double f = std::floor(x); return (x - f >= 0.5) ? f + 1 : f; }
inline double jsSign(double x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : x; }
inline double jsMod(double a, double b) { return b == 0 ? 0 : a - b * std::floor(a / b); }
inline int32_t toInt32(double d) {
  if (!std::isfinite(d)) return 0;
  const double t = std::trunc(d);
  double m = std::fmod(t, 4294967296.0);
  if (m < 0) m += 4294967296.0;
  return static_cast<int32_t>(static_cast<uint32_t>(m));
}
const double kTau = kPi * 2;
const double kLn2 = 0.6931471805599453;
const double kLn10 = 2.302585092994046;

// A running copy of a program: its own state and buffers.
class Runner {
public:
  Runner(const Program* prog, double sr) : prog_(prog), sr_(sr) { reset(); }

  void reset() {
    st_.assign(static_cast<size_t>(std::max(1, prog_->nState)), 0.0);
    buf_.clear();
    for (size_t n : prog_->buffers) buf_.emplace_back(n, 0.0f);
    for (const auto& i : prog_->inits) st_[static_cast<size_t>(i.first)] = i.second;
    loc_.assign(static_cast<size_t>(prog_->nLocals), 0.0);
  }

  void run(const float* in1, const float* in2, float* out1, float* out2, int n, const double* par, const double* N) {
    for (int k = 0; k < 4; k++) nt_[k] = N[k];
    trig_ = N[4];
    par_ = par;
    for (int i = 0; i < n; i++) {
      i_ = i;
      in_[0] = in1[i];
      in_[1] = in2[i];
      out_[0] = out_[1] = 0;
      for (auto& v : loc_) v = 0;
      exec(prog_->code);
      out1[i] = static_cast<float>(fin(out_[0]));
      out2[i] = static_cast<float>(fin(out_[1]));
    }
    for (auto& s : st_) if (!(s > -1e30 && s < 1e30)) s = 0;
  }

private:
  const Program* prog_;
  double sr_;
  std::vector<double> st_;
  std::vector<std::vector<float>> buf_;
  std::vector<double> loc_;
  double nt_[4] = {0, 0, 0, 0};
  double trig_ = 0;
  double in_[2] = {0, 0}, out_[2] = {0, 0};
  const double* par_ = nullptr;
  int i_ = 0;

  void assign(const Target& t, double v) {
    if (t.t == Target::Out) out_[t.idx] = v;
    else if (t.t == Target::Hist) st_[static_cast<size_t>(t.idx)] = v;
    else loc_[static_cast<size_t>(t.idx)] = v;
  }

  void exec(const std::vector<StmtP>& list) {
    for (const auto& s : list) {
      switch (s->t) {
        case Stmt::Set: { const double v = eval(*s->value); assign(s->target, v); break; }
        case Stmt::Tuple: {
          double tmp[16];
          std::vector<double> big;
          double* t = tmp;
          if (s->values.size() > 16) { big.resize(s->values.size()); t = big.data(); }
          for (size_t k = 0; k < s->values.size(); k++) t[k] = eval(*s->values[k]);
          for (size_t k = 0; k < s->targets.size(); k++) assign(s->targets[k], t[k]);
          break;
        }
        case Stmt::If: {
          if (eval(*s->cond) != 0) exec(s->then); else exec(s->other);
          break;
        }
        case Stmt::Expr: eval(*s->value); break;
      }
    }
  }

  double peekAt(int b, double i) {
    const auto& v = buf_[static_cast<size_t>(b)];
    const double n = static_cast<double>(v.size());
    const double idx = std::fmod(std::fmod(std::floor(i), n) + n, n);
    if (idx != idx) return std::nan("");
    return v[static_cast<size_t>(idx)];
  }
  void pokeAt(int b, double x, double i) {
    auto& v = buf_[static_cast<size_t>(b)];
    const double n = static_cast<double>(v.size());
    const double idx = std::fmod(std::fmod(std::floor(i), n) + n, n);
    if (idx != idx) return;
    v[static_cast<size_t>(idx)] = static_cast<float>(x);
  }

  double eval(const E& e) {
    switch (e.t) {
      case E::Num: return e.num;
      case E::In: return in_[e.idx];
      case E::Note: return nt_[e.idx];
      case E::Trig: return i_ == 0 ? trig_ : 0;
      case E::Out: return out_[e.idx];
      case E::Param: return par_[e.idx];
      case E::Hist: return st_[static_cast<size_t>(e.idx)];
      case E::Local: return loc_[static_cast<size_t>(e.idx)];
      case E::Neg: { const double x = eval(*e.k[0]); return -x; }
      case E::Not: { const double x = eval(*e.k[0]); return x == 0 ? 1 : 0; }
      case E::And: { const double l = eval(*e.k[0]); if (l == 0) return 0; const double r = eval(*e.k[1]); return r != 0 ? 1 : 0; }
      case E::Or: { const double l = eval(*e.k[0]); if (l != 0) return 1; const double r = eval(*e.k[1]); return r != 0 ? 1 : 0; }
      case E::Cond: { const double c = eval(*e.k[0]); return c != 0 ? eval(*e.k[1]) : eval(*e.k[2]); }
      case E::Div: case E::Mod: case E::Pow: case E::Lt: case E::Gt: case E::Le: case E::Ge: case E::Eq: case E::Ne:
      case E::Add: case E::Sub: case E::Mul: case E::RawDiv: {
        const double l = eval(*e.k[0]);
        const double r = eval(*e.k[1]);
        switch (e.t) {
          case E::Div: return r == 0 ? 0 : l / r;
          case E::Mod: return jsMod(l, r);
          case E::Pow: return fin(js::pow(l, r));
          case E::Lt: return l < r ? 1 : 0;
          case E::Gt: return l > r ? 1 : 0;
          case E::Le: return l <= r ? 1 : 0;
          case E::Ge: return l >= r ? 1 : 0;
          case E::Eq: return l == r ? 1 : 0;
          case E::Ne: return l != r ? 1 : 0;
          case E::Add: return l + r;
          case E::Sub: return l - r;
          case E::Mul: return l * r;
          default: return l / r;
        }
      }
      case E::Peek: { const double i = eval(*e.k[0]); return peekAt(e.idx, i); }
      case E::Poke: case E::PokeVoid: {
        const double x = eval(*e.k[0]);
        const double i = eval(*e.k[1]);
        pokeAt(e.idx, x, i);
        return e.t == E::Poke ? std::nan("") : 0;
      }
      case E::DelayWrite: {
        const double x = eval(*e.k[0]);
        auto& b = buf_[static_cast<size_t>(e.idx)];
        double& w = st_[static_cast<size_t>(e.w)];
        b[static_cast<size_t>(w)] = static_cast<float>(x);
        w = std::fmod(w + 1, static_cast<double>(b.size()));
        return 0;
      }
      case E::DelayRead: {
        const double t = eval(*e.k[0]);
        const auto& b = buf_[static_cast<size_t>(e.idx)];
        const double n = static_cast<double>(b.size());
        double pos = st_[static_cast<size_t>(e.w)] - jsMax(1, t);
        pos = std::fmod(std::fmod(pos, n) + n, n);
        if (pos != pos) return std::nan("");
        const double i0 = std::floor(pos);
        const double f = pos - i0;
        const double a = b[static_cast<size_t>(i0)];
        const double c = b[static_cast<size_t>(std::fmod(i0 + 1, n))];
        return a * (1 - f) + c * f;
      }
      case E::Func: return func(e);
    }
    return 0;
  }

  double func(const E& e) {
    double a[5] = {0, 0, 0, 0, 0};
    for (size_t j = 0; j < e.k.size(); j++) a[j] = eval(*e.k[j]);
    const size_t k = static_cast<size_t>(e.idx);
    switch (e.fn) {
      case Fn::Sin: return js::sin(a[0]);
      case Fn::Cos: return js::cos(a[0]);
      case Fn::Tan: return js::tan(a[0]);
      case Fn::Asin: return js::asin(jsClamp(a[0], -1, 1));
      case Fn::Acos: return js::acos(jsClamp(a[0], -1, 1));
      case Fn::Atan: return js::atan(a[0]);
      case Fn::Atan2: return js::atan2(a[0], a[1]);
      case Fn::Sinh: return js::sinh(a[0]);
      case Fn::Cosh: return js::cosh(a[0]);
      case Fn::Tanh: return js::tanh(a[0]);
      case Fn::Abs: return std::fabs(a[0]);
      case Fn::Sign: return jsSign(a[0]);
      case Fn::Floor: return std::floor(a[0]);
      case Fn::Ceil: return std::ceil(a[0]);
      case Fn::Round: return jsRoundX(a[0]);
      case Fn::Trunc: return std::trunc(a[0]);
      case Fn::Fract: return a[0] - std::floor(a[0]);
      case Fn::Sqrt: return a[0] > 0 ? std::sqrt(a[0]) : 0;
      case Fn::Exp: return fin(js::exp(a[0]));
      case Fn::Log: return a[0] > 0 ? js::log(a[0]) : 0;
      case Fn::Log2: return (a[0] > 0 ? js::log(a[0]) : 0) / kLn2;
      case Fn::Log10: return (a[0] > 0 ? js::log(a[0]) : 0) / kLn10;
      case Fn::Pow: return fin(js::pow(a[0], a[1]));
      case Fn::Min: return jsMin(a[0], a[1]);
      case Fn::Max: return jsMax(a[0], a[1]);
      case Fn::Clamp: return jsClamp(a[0], a[1], a[2]);
      case Fn::Mix: return a[0] + (a[1] - a[0]) * a[2];
      case Fn::Scale: return a[2] == a[1] ? a[3] : a[3] + (a[0] - a[1]) * (a[4] - a[3]) / (a[2] - a[1]);
      case Fn::Wrap: { const double r = a[2] - a[1]; return r == 0 ? a[1] : a[1] + jsMod(a[0] - a[1], r); }
      case Fn::Fold: {
        const double r = a[2] - a[1];
        if (r == 0) return a[1];
        const double t = jsMod(a[0] - a[1], 2 * r);
        return a[1] + (t > r ? 2 * r - t : t);
      }
      case Fn::Mod: return jsMod(a[0], a[1]);
      case Fn::Mtof: return 440 * js::pow(2, (a[0] - 69) / 12);
      case Fn::Ftom: return 69 + 12 * (a[0] / 440 > 0 ? js::log(a[0] / 440) : 0) / kLn2;
      case Fn::Dbtoa: return js::pow(10, a[0] / 20);
      case Fn::Atodb: { const double m = jsMax(a[0], 1e-12); return 20 * (m > 0 ? js::log(m) : 0) / kLn10; }
      case Fn::Mstosamps: return a[0] * sr_ / 1000;
      case Fn::Sampstoms: return a[0] * 1000 / sr_;
      case Fn::Fixdenorm: return (a[0] > -1e-18 && a[0] < 1e-18) ? 0 : a[0];
      case Fn::Noise: {
        int32_t x = toInt32(st_[k]);
        if (x == 0) x = 22222;
        uint32_t u = static_cast<uint32_t>(x);
        u ^= u << 13;
        u ^= u >> 17;
        u ^= u << 5;
        st_[k] = static_cast<double>(static_cast<int32_t>(u));
        return static_cast<double>(u) / 2147483648.0 - 1;
      }
      case Fn::Phasor: {
        const double p = st_[k];
        double q = p + a[0] / sr_;
        q -= std::floor(q);
        st_[k] = q;
        return p;
      }
      case Fn::Cycle: {
        const double p = st_[k];
        double q = p + a[0] / sr_;
        q -= std::floor(q);
        st_[k] = q;
        return js::sin(kTau * p);
      }
      case Fn::Delta: { const double d = a[0] - st_[k]; st_[k] = a[0]; return d; }
      case Fn::Change: { const double d = a[0] - st_[k]; st_[k] = a[0]; return jsSign(d); }
      case Fn::Slide: {
        const double y = st_[k];
        const double s = jsMax(1, a[0] > y ? a[1] : a[2]);
        st_[k] = y + (a[0] - y) / s;
        return st_[k];
      }
      case Fn::Dcblock: {
        const double y = a[0] - st_[k] + 0.995 * st_[k + 1];
        st_[k] = a[0];
        st_[k + 1] = y;
        return y;
      }
    }
    return 0;
  }
};

// ---------------------------------------------------------------------------------------------------------- node
class GenExpr final : public Node {
public:
  explicit GenExpr(const NodeContext& context) : sr_(context.sampleRate) {
    std::string code = kDefaultCode;
    if (context.data && context.data->is_object() && context.data->contains("code") && (*context.data)["code"].is_string())
      code = (*context.data)["code"].get<std::string>();
    try {
      Compiler c(sr_);
      prog_ = c.compile(code);
      run_ = std::make_unique<Runner>(prog_.get(), sr_);
    } catch (const CompileError&) {
      prog_.reset();
    }
  }

  void note(const NoteEvent& e) override {
    if (e.type == NoteType::On) {
      drop(e.note);
      held_.push_back(e.note);
      N_[2] = e.vel;
      N_[4] = 1;
      aim();
    } else if (e.type == NoteType::Off) {
      drop(e.note);
      aim();
    } else if (e.type == NoteType::AllOff) {
      held_.clear();
      aim();
    } else {  // a cut starts the code again from nothing
      held_.clear();
      aim();
      if (run_) run_->reset();
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    if (!run_) {
      for (int i = 0; i < n; i++) outputs[0][i] = outputs[1][i] = 0;
      return;
    }
    for (size_t i = 0; i < prog_->params.size(); i++) {
      const auto& m = prog_->params[i];
      P_[i] = m.min + static_cast<double>(params[slots[i]]) * (m.max - m.min);
    }
    run_->run(inputs[0], inputs[1], outputs[0], outputs[1], n, P_, N_);
    N_[4] = 0;
  }

private:
  double sr_;
  std::unique_ptr<Program> prog_;
  std::unique_ptr<Runner> run_;
  double P_[kMaxParams] = {0, 0, 0, 0, 0, 0, 0, 0};
  double N_[5] = {440, 0, 1, 69, 0};  // freq, gate, vel, note, trig
  std::vector<int> held_;

  void drop(int note) {
    for (size_t i = 0; i < held_.size(); i++) if (held_[i] == note) { held_.erase(held_.begin() + static_cast<long>(i)); break; }
  }
  void aim() {  // the note the code hears: the latest key still down, else the last one that was
    if (!held_.empty()) {
      const int n = held_.back();
      N_[3] = n;
      N_[0] = 440 * js::pow(2, (static_cast<double>(n) - 69) / 12);
    }
    N_[1] = held_.empty() ? 0 : 1;
  }
};

}  // namespace

const char* genexprDefaultCode() { return kDefaultCode; }

GenExprReport checkGenExpr(const std::string& code, double sampleRate) {
  GenExprReport report;
  try {
    Compiler compiler(sampleRate);
    const auto program = compiler.compile(code);
    report.ok = true;
    for (std::size_t i = 0; i < program->params.size(); i++) report.params.push_back({program->paramNames[i], program->params[i].min, program->params[i].max, program->params[i].def});
  } catch (const CompileError& e) {
    report.errors.push_back({e.at.line, e.at.col, std::max(e.at.end, e.at.col + 1), e.message});
  }
  return report;
}

void registerGenExpr(NodeRegistry& registry) {
  NodeType type;
  type.type = "genexpr";
  type.inputs = {"in1", "in2"};
  type.outputs = {"out1", "out2"};
  type.kinds = {{"in1", PortKind::Audio}, {"in2", PortKind::Audio}, {"out1", PortKind::Audio}, {"out2", PortKind::Audio}};
  for (int i = 1; i <= kMaxParams; i++) type.params.push_back({"p" + std::to_string(i), 0, 1, 0.5});
  type.isEffect = true;
  type.create = [](const NodeContext& context) { return std::make_unique<GenExpr>(context); };
  registry.add(std::move(type));
}

}  // namespace wax
