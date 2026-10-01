//===- main.cpp - Interactive LLVM KnownBits transfer explorer ------------===//

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

enum class Opcode {
  And,
  Or,
  Xor,
  Add,
  Sub,
  Mul,
  SDiv,
  UDiv,
  SRem,
  URem,
  Shl,
  LShr,
  AShr,
  UMin,
  UMax,
  SMin,
  SMax,
  Trunc,
  ZExt,
  SExt,
};

struct Expression {
  Opcode opcode;
  llvm::KnownBits lhs;
  llvm::KnownBits rhs;
  bool nsw = false;
  bool nuw = false;
  bool exact = false;
  bool self = false;
  bool sameOperands = false;
  bool compare = false;
  bool exhaust = false;
  unsigned targetWidth = 0;
  std::string resultName;
  std::string source;
};

using VariableMap = std::unordered_map<std::string, llvm::KnownBits>;

std::string pattern(const llvm::KnownBits &bits) {
  std::string result;
  result.reserve(bits.getBitWidth());
  for (unsigned i = bits.getBitWidth(); i > 0; --i) {
    unsigned bit = i - 1;
    if (bits.Zero[bit] && bits.One[bit])
      result.push_back('!');
    else if (bits.Zero[bit])
      result.push_back('0');
    else if (bits.One[bit])
      result.push_back('1');
    else
      result.push_back('?');
  }
  return result;
}

std::string bitMask(const llvm::APInt &value) {
  std::string result;
  result.reserve(value.getBitWidth());
  for (unsigned i = value.getBitWidth(); i > 0; --i)
    result.push_back(value[i - 1] ? '1' : '0');
  return result;
}

std::string decimal(const llvm::APInt &value, bool isSigned) {
  return llvm::toString(value, 10, isSigned);
}

unsigned unknownBitCount(const llvm::KnownBits &bits) {
  return (~(bits.Zero | bits.One)).popcount();
}

bool isPattern(llvm::StringRef token) {
  return !token.empty() && llvm::all_of(token, [](char c) {
    return c == '0' || c == '1' || c == '?';
  });
}

std::optional<llvm::KnownBits> parsePattern(llvm::StringRef text,
                                            std::string &error) {
  if (!isPattern(text)) {
    error = "expected a bit pattern containing only 0, 1, and ?";
    return std::nullopt;
  }

  llvm::KnownBits result(text.size());
  for (unsigned textualIndex = 0; textualIndex < text.size(); ++textualIndex) {
    unsigned bit = text.size() - textualIndex - 1;
    if (text[textualIndex] == '0')
      result.Zero.setBit(bit);
    else if (text[textualIndex] == '1')
      result.One.setBit(bit);
  }
  return result;
}

bool isVariable(llvm::StringRef token) {
  return token.size() > 1 && token.front() == '%';
}

std::optional<llvm::KnownBits> parseOperand(llvm::StringRef token,
                                            const VariableMap &variables,
                                            std::string &error) {
  if (!token.starts_with("%"))
    return parsePattern(token, error);
  if (!isVariable(token)) {
    error = "expected a variable name after '%'";
    return std::nullopt;
  }

  auto variable = variables.find(token.str());
  if (variable == variables.end()) {
    error = "undefined variable '" + token.str() + "'";
    return std::nullopt;
  }
  return variable->second;
}

std::optional<Opcode> parseOpcode(llvm::StringRef token) {
  if (token == "and")
    return Opcode::And;
  if (token == "or")
    return Opcode::Or;
  if (token == "xor")
    return Opcode::Xor;
  if (token == "add")
    return Opcode::Add;
  if (token == "sub")
    return Opcode::Sub;
  if (token == "mul")
    return Opcode::Mul;
  if (token == "sdiv")
    return Opcode::SDiv;
  if (token == "udiv")
    return Opcode::UDiv;
  if (token == "srem")
    return Opcode::SRem;
  if (token == "urem")
    return Opcode::URem;
  if (token == "shl")
    return Opcode::Shl;
  if (token == "lshr")
    return Opcode::LShr;
  if (token == "ashr")
    return Opcode::AShr;
  if (token == "umin")
    return Opcode::UMin;
  if (token == "umax")
    return Opcode::UMax;
  if (token == "smin")
    return Opcode::SMin;
  if (token == "smax")
    return Opcode::SMax;
  if (token == "trunc")
    return Opcode::Trunc;
  if (token == "zext")
    return Opcode::ZExt;
  if (token == "sext")
    return Opcode::SExt;
  return std::nullopt;
}

bool isResize(Opcode opcode) {
  return opcode == Opcode::Trunc || opcode == Opcode::ZExt ||
         opcode == Opcode::SExt;
}

bool isShift(Opcode opcode) {
  return opcode == Opcode::Shl || opcode == Opcode::LShr ||
         opcode == Opcode::AShr;
}

bool acceptsNoWrapFlags(Opcode opcode) {
  return opcode == Opcode::Add || opcode == Opcode::Sub ||
         opcode == Opcode::Shl;
}

bool acceptsExactFlag(Opcode opcode) {
  return opcode == Opcode::SDiv || opcode == Opcode::UDiv ||
         opcode == Opcode::LShr || opcode == Opcode::AShr;
}

std::optional<Expression> parseExpression(llvm::StringRef input,
                                          const VariableMap &variables,
                                          std::string &error) {
  std::string source = input.trim().str();
  std::string tokenizable = source;
  std::replace(tokenizable.begin(), tokenizable.end(), ',', ' ');

  std::istringstream stream(tokenizable);
  std::vector<std::string> tokens;
  for (std::string token; stream >> token;)
    tokens.push_back(std::move(token));

  if (tokens.empty()) {
    error = "expected an operation";
    return std::nullopt;
  }

  unsigned cursor = 0;
  std::string resultName;
  if (llvm::StringRef(tokens[cursor]).starts_with("%")) {
    if (!isVariable(tokens[cursor])) {
      error = "expected a variable name after '%'";
      return std::nullopt;
    }
    if (tokens.size() < 2 || tokens[1] != "=") {
      error = "expected '=' after variable '" + tokens[cursor] + "'";
      return std::nullopt;
    }
    resultName = tokens[cursor];
    if (variables.count(resultName)) {
      error = "variable '" + resultName + "' is already defined";
      return std::nullopt;
    }
    cursor = 2;
    if (cursor == tokens.size()) {
      error = "expected an operation after '='";
      return std::nullopt;
    }
  }

  bool compare = false;
  bool exhaust = false;
  if (tokens[cursor] == "compare") {
    compare = true;
    ++cursor;
  } else if (tokens[cursor] == "exhaust") {
    exhaust = true;
    ++cursor;
  }
  if (cursor == tokens.size()) {
    error = compare ? "expected an operation after 'compare'"
                    : "expected an operation after 'exhaust'";
    return std::nullopt;
  }

  std::optional<Opcode> opcode = parseOpcode(tokens[cursor++]);
  if (!opcode) {
    error = "unknown operation '" + tokens[cursor - 1] + "'";
    return std::nullopt;
  }
  if (compare && !acceptsNoWrapFlags(*opcode)) {
    error = "'compare' is only available for add, sub, and shl";
    return std::nullopt;
  }
  if (compare && !resultName.empty()) {
    error = "a compare expression cannot be assigned to a variable";
    return std::nullopt;
  }
  if (exhaust && !resultName.empty()) {
    error = "an exhaust expression cannot be assigned to a variable";
    return std::nullopt;
  }

  bool nsw = false;
  bool nuw = false;
  bool exact = false;
  bool self = false;
  while (cursor < tokens.size() && !isPattern(tokens[cursor]) &&
         !llvm::StringRef(tokens[cursor]).starts_with("%")) {
    llvm::StringRef flag(tokens[cursor++]);
    if (flag == "nsw") {
      if (nsw) {
        error = "duplicate 'nsw' flag";
        return std::nullopt;
      }
      nsw = true;
    } else if (flag == "nuw") {
      if (nuw) {
        error = "duplicate 'nuw' flag";
        return std::nullopt;
      }
      nuw = true;
    } else if (flag == "exact") {
      if (exact) {
        error = "duplicate 'exact' flag";
        return std::nullopt;
      }
      exact = true;
    } else if (flag == "self") {
      if (self) {
        error = "duplicate 'self' flag";
        return std::nullopt;
      }
      self = true;
    } else {
      error = "unknown flag '" + flag.str() + "'";
      return std::nullopt;
    }
  }

  if ((nsw || nuw) && !acceptsNoWrapFlags(*opcode)) {
    error = "nuw and nsw are only valid for add, sub, and shl";
    return std::nullopt;
  }
  if (exact && !acceptsExactFlag(*opcode)) {
    error = "exact is only valid for sdiv, udiv, lshr, and ashr";
    return std::nullopt;
  }
  if (compare && (nsw || nuw)) {
    error = "compare supplies its own flag combinations";
    return std::nullopt;
  }
  if (self && *opcode != Opcode::Add) {
    error = "self is currently supported only for add";
    return std::nullopt;
  }

  unsigned targetWidth = 0;
  llvm::StringRef lhsToken;
  llvm::StringRef rhsToken;
  std::optional<llvm::KnownBits> lhs;
  std::optional<llvm::KnownBits> rhs;

  if (isResize(*opcode)) {
    if (tokens.size() - cursor != 3 || tokens[cursor + 1] != "to") {
      error = "resize operation expects: OPERAND to WIDTH";
      return std::nullopt;
    }
    lhsToken = tokens[cursor];
    lhs = parseOperand(lhsToken, variables, error);
    if (!lhs)
      return std::nullopt;
    if (llvm::StringRef(tokens[cursor + 2]).getAsInteger(10, targetWidth) ||
        targetWidth == 0) {
      error = "target width must be a positive integer";
      return std::nullopt;
    }
    if (*opcode == Opcode::Trunc && targetWidth >= lhs->getBitWidth()) {
      error = "trunc target width must be smaller than the input width";
      return std::nullopt;
    }
    if ((*opcode == Opcode::ZExt || *opcode == Opcode::SExt) &&
        targetWidth <= lhs->getBitWidth()) {
      error = "extension target width must be larger than the input width";
      return std::nullopt;
    }
    rhs = lhs;
  } else {
    unsigned expectedOperands = self ? 1 : 2;
    if (tokens.size() - cursor != expectedOperands) {
      error = self ? "self add expects one operand"
                   : "binary operation expects two operands";
      return std::nullopt;
    }

    lhsToken = tokens[cursor];
    lhs = parseOperand(lhsToken, variables, error);
    if (!lhs)
      return std::nullopt;

    rhsToken = self ? lhsToken : llvm::StringRef(tokens[cursor + 1]);
    rhs = self ? lhs : parseOperand(rhsToken, variables, error);
    if (!rhs)
      return std::nullopt;
    if (!isShift(*opcode) && lhs->getBitWidth() != rhs->getBitWidth()) {
      error = "operand bit widths do not match";
      return std::nullopt;
    }
  }

  bool sameVariable =
      !rhsToken.empty() && isVariable(lhsToken) && lhsToken == rhsToken;
  bool useSelf = self || (*opcode == Opcode::Add && sameVariable);
  bool sameOperands = self || sameVariable;
  if (exhaust) {
    unsigned unknownBits = unknownBitCount(*lhs);
    if (!isResize(*opcode) && !sameOperands)
      unknownBits += unknownBitCount(*rhs);
    if (unknownBits > 16) {
      error = "exhaust supports at most 16 total unknown input bits";
      return std::nullopt;
    }
  }
  return Expression{*opcode,
                    std::move(*lhs),
                    std::move(*rhs),
                    nsw,
                    nuw,
                    exact,
                    useSelf,
                    sameOperands,
                    compare,
                    exhaust,
                    targetWidth,
                    std::move(resultName),
                    std::move(source)};
}

llvm::KnownBits evaluate(const Expression &expression, bool nsw, bool nuw) {
  switch (expression.opcode) {
  case Opcode::And:
    return expression.lhs & expression.rhs;
  case Opcode::Or:
    return expression.lhs | expression.rhs;
  case Opcode::Xor:
    return expression.lhs ^ expression.rhs;
  case Opcode::Add:
    return llvm::KnownBits::add(expression.lhs, expression.rhs, nsw, nuw,
                                expression.self);
  case Opcode::Sub:
    return llvm::KnownBits::sub(expression.lhs, expression.rhs, nsw, nuw);
  case Opcode::Mul:
    return llvm::KnownBits::mul(expression.lhs, expression.rhs,
                                expression.sameOperands);
  case Opcode::SDiv:
    return llvm::KnownBits::sdiv(expression.lhs, expression.rhs,
                                 expression.exact);
  case Opcode::UDiv:
    return llvm::KnownBits::udiv(expression.lhs, expression.rhs,
                                 expression.exact);
  case Opcode::SRem:
    return llvm::KnownBits::srem(expression.lhs, expression.rhs);
  case Opcode::URem:
    return llvm::KnownBits::urem(expression.lhs, expression.rhs);
  case Opcode::Shl:
    return llvm::KnownBits::shl(expression.lhs, expression.rhs, nuw, nsw,
                                expression.rhs.isNonZero());
  case Opcode::LShr:
    return llvm::KnownBits::lshr(expression.lhs, expression.rhs,
                                 expression.rhs.isNonZero(), expression.exact);
  case Opcode::AShr:
    return llvm::KnownBits::ashr(expression.lhs, expression.rhs,
                                 expression.rhs.isNonZero(), expression.exact);
  case Opcode::UMin:
    return llvm::KnownBits::umin(expression.lhs, expression.rhs);
  case Opcode::UMax:
    return llvm::KnownBits::umax(expression.lhs, expression.rhs);
  case Opcode::SMin:
    return llvm::KnownBits::smin(expression.lhs, expression.rhs);
  case Opcode::SMax:
    return llvm::KnownBits::smax(expression.lhs, expression.rhs);
  case Opcode::Trunc:
    return expression.lhs.trunc(expression.targetWidth);
  case Opcode::ZExt:
    return expression.lhs.zext(expression.targetWidth);
  case Opcode::SExt:
    return expression.lhs.sext(expression.targetWidth);
  }
  llvm_unreachable("all opcodes handled");
}

std::optional<llvm::APInt> evaluateConcrete(const Expression &expression,
                                            const llvm::APInt &lhs,
                                            const llvm::APInt &rhs) {
  bool overflow = false;
  switch (expression.opcode) {
  case Opcode::And:
    return lhs & rhs;
  case Opcode::Or:
    return lhs | rhs;
  case Opcode::Xor:
    return lhs ^ rhs;
  case Opcode::Add: {
    llvm::APInt result = lhs + rhs;
    if (expression.nuw) {
      result = lhs.uadd_ov(rhs, overflow);
      if (overflow)
        return std::nullopt;
    }
    if (expression.nsw) {
      result = lhs.sadd_ov(rhs, overflow);
      if (overflow)
        return std::nullopt;
    }
    return result;
  }
  case Opcode::Sub: {
    llvm::APInt result = lhs - rhs;
    if (expression.nuw) {
      result = lhs.usub_ov(rhs, overflow);
      if (overflow)
        return std::nullopt;
    }
    if (expression.nsw) {
      result = lhs.ssub_ov(rhs, overflow);
      if (overflow)
        return std::nullopt;
    }
    return result;
  }
  case Opcode::Mul:
    return lhs * rhs;
  case Opcode::SDiv:
    if (rhs.isZero() || (lhs.isMinSignedValue() && rhs.isAllOnes()))
      return std::nullopt;
    if (expression.exact && !lhs.srem(rhs).isZero())
      return std::nullopt;
    return lhs.sdiv(rhs);
  case Opcode::UDiv:
    if (rhs.isZero())
      return std::nullopt;
    if (expression.exact && !lhs.urem(rhs).isZero())
      return std::nullopt;
    return lhs.udiv(rhs);
  case Opcode::SRem:
    if (rhs.isZero() || (lhs.isMinSignedValue() && rhs.isAllOnes()))
      return std::nullopt;
    return lhs.srem(rhs);
  case Opcode::URem:
    if (rhs.isZero())
      return std::nullopt;
    return lhs.urem(rhs);
  case Opcode::Shl: {
    uint64_t amount = rhs.getLimitedValue(lhs.getBitWidth());
    if (amount >= lhs.getBitWidth())
      return std::nullopt;
    llvm::APInt result = lhs.shl(static_cast<unsigned>(amount));
    if (expression.nuw) {
      result = lhs.ushl_ov(static_cast<unsigned>(amount), overflow);
      if (overflow)
        return std::nullopt;
    }
    if (expression.nsw) {
      result = lhs.sshl_ov(static_cast<unsigned>(amount), overflow);
      if (overflow)
        return std::nullopt;
    }
    return result;
  }
  case Opcode::LShr:
  case Opcode::AShr: {
    uint64_t amount = rhs.getLimitedValue(lhs.getBitWidth());
    if (amount >= lhs.getBitWidth())
      return std::nullopt;
    if (expression.exact && amount != 0 &&
        !lhs.getLoBits(static_cast<unsigned>(amount)).isZero())
      return std::nullopt;
    return expression.opcode == Opcode::LShr
               ? lhs.lshr(static_cast<unsigned>(amount))
               : lhs.ashr(static_cast<unsigned>(amount));
  }
  case Opcode::UMin:
    return lhs.ult(rhs) ? lhs : rhs;
  case Opcode::UMax:
    return lhs.ugt(rhs) ? lhs : rhs;
  case Opcode::SMin:
    return lhs.slt(rhs) ? lhs : rhs;
  case Opcode::SMax:
    return lhs.sgt(rhs) ? lhs : rhs;
  case Opcode::Trunc:
    return lhs.trunc(expression.targetWidth);
  case Opcode::ZExt:
    return lhs.zext(expression.targetWidth);
  case Opcode::SExt:
    return lhs.sext(expression.targetWidth);
  }
  llvm_unreachable("all opcodes handled");
}

std::vector<llvm::APInt> enumerateValues(const llvm::KnownBits &bits) {
  std::vector<unsigned> unknownBits;
  for (unsigned bit = 0; bit < bits.getBitWidth(); ++bit)
    if (!bits.Zero[bit] && !bits.One[bit])
      unknownBits.push_back(bit);

  uint64_t count = uint64_t{1} << unknownBits.size();
  std::vector<llvm::APInt> values;
  values.reserve(count);
  for (uint64_t choice = 0; choice < count; ++choice) {
    llvm::APInt value = bits.One;
    for (unsigned index = 0; index < unknownBits.size(); ++index)
      if (choice & (uint64_t{1} << index))
        value.setBit(unknownBits[index]);
    values.push_back(std::move(value));
  }
  return values;
}

struct APIntLess {
  bool operator()(const llvm::APInt &lhs, const llvm::APInt &rhs) const {
    return lhs.ult(rhs);
  }
};

llvm::KnownBits bestKnownBits(const std::set<llvm::APInt, APIntLess> &values) {
  llvm::APInt knownOne = *values.begin();
  llvm::APInt knownZero = ~knownOne;
  for (const llvm::APInt &value : values) {
    knownOne &= value;
    knownZero &= ~value;
  }
  llvm::KnownBits result(knownOne.getBitWidth());
  result.Zero = std::move(knownZero);
  result.One = std::move(knownOne);
  return result;
}

void printConcreteValues(llvm::raw_ostream &out,
                         const std::set<llvm::APInt, APIntLess> &values) {
  out << "{";
  auto iterator = values.begin();
  while (iterator != values.end()) {
    llvm::APInt first = *iterator;
    llvm::APInt last = first;
    ++iterator;
    while (iterator != values.end() && *iterator == last + 1) {
      last = *iterator;
      ++iterator;
    }
    out << decimal(first, false);
    if (last != first)
      out << ".." << decimal(last, false);
    if (iterator != values.end())
      out << ", ";
  }
  out << "}";
}

void printValue(llvm::raw_ostream &out, llvm::StringRef label,
                const llvm::KnownBits &value,
                const llvm::APInt *gained = nullptr) {
  out << "  " << label;
  out.indent(label.size() < 9 ? 9 - label.size() : 1);
  out << pattern(value) << "  u=[" << decimal(value.getMinValue(), false)
      << ", " << decimal(value.getMaxValue(), false) << "] s=["
      << decimal(value.getSignedMinValue(), true) << ", "
      << decimal(value.getSignedMaxValue(), true) << "]";
  if (gained)
    out << " gained=" << bitMask(*gained);
  out << "\n";
}

void printExhaust(llvm::raw_ostream &out, const Expression &expression) {
  llvm::KnownBits llvmResult =
      evaluate(expression, expression.nsw, expression.nuw);
  printValue(out, "llvm", llvmResult);

  std::vector<llvm::APInt> lhsValues = enumerateValues(expression.lhs);
  std::vector<llvm::APInt> rhsValues =
      isResize(expression.opcode) || expression.sameOperands
          ? std::vector<llvm::APInt>{}
          : enumerateValues(expression.rhs);
  uint64_t total = expression.sameOperands || isResize(expression.opcode)
                       ? lhsValues.size()
                       : lhsValues.size() * rhsValues.size();
  uint64_t excluded = 0;
  std::set<llvm::APInt, APIntLess> results;

  for (const llvm::APInt &lhs : lhsValues) {
    if (expression.sameOperands || isResize(expression.opcode)) {
      std::optional<llvm::APInt> result =
          evaluateConcrete(expression, lhs, lhs);
      if (result)
        results.insert(std::move(*result));
      else
        ++excluded;
      continue;
    }
    for (const llvm::APInt &rhs : rhsValues) {
      std::optional<llvm::APInt> result =
          evaluateConcrete(expression, lhs, rhs);
      if (result)
        results.insert(std::move(*result));
      else
        ++excluded;
    }
  }

  out << "  defined  " << total - excluded << "/" << total;
  if (excluded)
    out << " (" << excluded << " poison/undefined excluded)";
  out << "\n";
  if (results.empty()) {
    out << "  values   {}\n";
    return;
  }

  out << "  values   ";
  printConcreteValues(out, results);
  out << "\n";
  llvm::KnownBits best = bestKnownBits(results);
  printValue(out, "best", best);
  llvm::APInt llvmKnown = llvmResult.Zero | llvmResult.One;
  llvm::APInt bestKnown = best.Zero | best.One;
  llvm::APInt missed = bestKnown & ~llvmKnown;
  out << "  missed   " << bitMask(missed) << "\n";
  llvm::APInt unsound =
      (llvmResult.Zero & ~best.Zero) | (llvmResult.One & ~best.One);
  if (!unsound.isZero())
    out << "  UNSOUND  " << bitMask(unsound) << "\n";
}

std::optional<llvm::KnownBits> printExpression(llvm::raw_ostream &out,
                                               const Expression &expression) {
  out << expression.source << "\n";
  if (isResize(expression.opcode)) {
    printValue(out, "input", expression.lhs);
  } else {
    printValue(out, "lhs", expression.lhs);
    printValue(out, expression.self ? "rhs=same" : "rhs", expression.rhs);
  }

  if (expression.exhaust) {
    printExhaust(out, expression);
    return std::nullopt;
  }

  if (expression.compare) {
    struct Variant {
      llvm::StringLiteral label;
      bool nsw;
      bool nuw;
    };
    constexpr Variant variants[] = {{"plain", false, false},
                                    {"nuw", false, true},
                                    {"nsw", true, false},
                                    {"nuw nsw", true, true}};
    llvm::KnownBits baseline = evaluate(expression, false, false);
    llvm::APInt baselineKnown = baseline.Zero | baseline.One;
    for (const Variant &variant : variants) {
      llvm::KnownBits result = evaluate(expression, variant.nsw, variant.nuw);
      llvm::APInt gained = (result.Zero | result.One) & ~baselineKnown;
      printValue(out, variant.label, result, &gained);
    }
    return std::nullopt;
  }

  llvm::KnownBits result = evaluate(expression, expression.nsw, expression.nuw);
  printValue(out,
             expression.resultName.empty()
                 ? llvm::StringRef("result")
                 : llvm::StringRef(expression.resultName),
             result);
  out << "  zero     " << bitMask(result.Zero) << "\n"
      << "  one      " << bitMask(result.One) << "\n";
  return result;
}

void printHelp(llvm::raw_ostream &out, llvm::StringRef program) {
  out << "LLVM KnownBits transfer-function explorer\n\n"
      << "usage: " << program << " [FILE]\n"
      << "       " << program << " -e EXPRESSION\n\n"
      << "With no FILE, expressions are read from standard input. Use # for "
         "comments.\n\n"
      << "examples:\n"
      << "  add nuw 0000????, 1000????\n"
      << "  %x = and 10??0011, 11110000\n"
      << "  %y = add %x, 00000001\n"
      << "  shl nuw 0000????, 00000010\n"
      << "  lshr exact 10000000, 00000001\n"
      << "  %wide = zext %x to 16\n"
      << "  sub nsw 01??????, 00000001\n"
      << "  xor 10??0011, 11110000\n"
      << "  add self ????????\n"
      << "  compare add 1111????, 00000001\n"
      << "  exhaust udiv exact 0001????, 00000010\n\n"
      << "operations: and, or, xor, add, sub, mul, sdiv, udiv, srem, "
         "urem,\n"
      << "            shl, lshr, ashr, umin, umax, smin, smax, trunc, "
         "zext, sext\n"
      << "flags:      nuw, nsw, exact, self (self is for add only)\n";
}

int process(llvm::StringRef text, llvm::raw_ostream &out,
            llvm::raw_ostream &err) {
  int status = 0;
  unsigned lineNumber = 0;
  VariableMap variables;
  while (!text.empty()) {
    auto [rawLine, remainder] = text.split('\n');
    text = remainder;
    ++lineNumber;

    llvm::StringRef line = rawLine.split('#').first.trim();
    if (line.empty())
      continue;

    std::string parseError;
    std::optional<Expression> expression =
        parseExpression(line, variables, parseError);
    if (!expression) {
      err << "line " << lineNumber << ": error: " << parseError << "\n"
          << "  " << rawLine << "\n";
      status = 1;
      continue;
    }

    std::optional<llvm::KnownBits> result = printExpression(out, *expression);
    if (!expression->resultName.empty())
      variables.emplace(expression->resultName, std::move(*result));
  }
  return status;
}

} // namespace

int main(int argc, char **argv) {
  llvm::StringRef program = argc > 0 ? argv[0] : "known-bits-explorer";
  if (argc == 2 && llvm::StringRef(argv[1]) == "--help") {
    printHelp(llvm::outs(), program);
    return 0;
  }
  if (argc == 2 && llvm::StringRef(argv[1]) == "--version") {
    llvm::outs() << "known-bits-explorer (LLVM " << LLVM_VERSION_STRING
                 << ")\n";
    return 0;
  }

  std::string input;
  if (argc == 3 && llvm::StringRef(argv[1]) == "-e") {
    input = argv[2];
  } else if (argc == 1) {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    input = buffer.str();
  } else if (argc == 2) {
    std::ifstream file(argv[1]);
    if (!file) {
      llvm::errs() << program << ": error: could not open '" << argv[1]
                   << "'\n";
      return 1;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    input = buffer.str();
  } else {
    printHelp(llvm::errs(), program);
    return 1;
  }

  if (input.empty() || input.back() != '\n')
    input.push_back('\n');
  return process(input, llvm::outs(), llvm::errs());
}
