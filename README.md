# KnownBits Explorer

A small command-line interface to LLVM's real `KnownBits` transfer functions.
Write abstract bit patterns with `0` for known zero, `1` for known one, and `?`
for unknown, then see what LLVM can prove about the result.

```text
$ known-bits-explorer -e 'add nuw 0000????, 1000????'
add nuw 0000????, 1000????
  lhs      0000????  u=[0, 15] s=[0, 15]
  rhs      1000????  u=[128, 143] s=[-128, -113]
  result   100?????  u=[128, 159] s=[-128, -97]
  zero     01100000
  one      10000000
```

The explorer calls LLVM 23's `llvm::KnownBits` operations directly. It supports
`and`, `or`, `xor`, `add`, `sub`, `mul`, `sdiv`, `udiv`, `srem`, `urem`, `shl`,
`lshr`, `ashr`, `umin`, `umax`, `smin`, `smax`, `trunc`, `zext`, and `sext`.
The `nuw`, `nsw`, and `exact` flags are accepted by operations whose LLVM
transfer functions support them.

`compare` shows all four no-wrap flag combinations side by side. `gained` marks
bits that become known relative to the unflagged transfer:

```text
compare add 1111????, 00000001
  lhs      1111????  u=[240, 255] s=[-16, -1]
  rhs      00000001  u=[1, 1] s=[1, 1]
  plain    ????????  u=[0, 255] s=[-128, 127] gained=00000000
  nuw      1111????  u=[240, 255] s=[-16, -1] gained=11110000
  nsw      ????????  u=[0, 255] s=[-128, 127] gained=00000000
  nuw nsw  1111????  u=[240, 255] s=[-16, -1] gained=11110000
```

`add self PATTERN` distinguishes a value added to itself from two independent
values that happen to have the same pattern. This uses LLVM's dedicated
`SelfAdd` transfer path.

## Variables

Prefix an expression with `%name =` to bind its result. Every expression still
prints immediately, and later expressions can use the name wherever they could
use a bit pattern:

```text
%x = and 10??0011, 11110000
%y = add nuw %x, 00000001
%z = xor %y, 10000000
```

Variables are immutable and must be defined before use. Using the same variable
as both operands of `add` automatically uses LLVM's correlated `SelfAdd` path.
Other raw KnownBits transfers receive only two abstract values and cannot see
that `%x` and `%x` are the same SSA value; `exhaust` preserves that correlation
and can expose the resulting precision gap. `compare` and `exhaust` are
display-only and cannot be assigned.

## Exhaustive comparison

Prefix an expression with `exhaust` to enumerate all concrete inputs, compare
their exact results with LLVM's abstract result, and report any known bits LLVM
did not find:

```text
exhaust udiv exact 0001????, 00000010
  lhs      0001????  u=[16, 31] s=[16, 31]
  rhs      00000010  u=[2, 2] s=[2, 2]
  llvm     0000????  u=[0, 15] s=[0, 15]
  defined  8/16 (8 poison/undefined excluded)
  values   {8..15}
  best     00001???  u=[8, 15] s=[8, 15]
  missed   00001000
```

Concrete results are printed exactly as comma-separated values and inclusive
ranges. Executions made poison or undefined by no-wrap flags, `exact`, invalid
shift amounts, or invalid division are excluded. To keep the Cartesian product
manageable, `exhaust` accepts at most 16 total unknown input bits (65,536
concrete executions).

Width-changing operations use `OPERAND to WIDTH`:

```text
%wide = zext 10??0011 to 16
%low = trunc %wide to 8
```

## Build

LLVM 23 is required. With Nix:

```sh
nix develop
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

You can also build and run the packaged executable directly:

```sh
nix build
nix run . -- -e 'sub nsw 01??????, 00000001'
```

Without Nix, point CMake at an LLVM 23 installation if it is not already on
the search path:

```sh
cmake -S . -B build -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build
```

## Input

Pass one expression with `-e`, pass a file, or provide expressions on standard
input. Blank lines and text following `#` are ignored.

```text
and 10??0011, 11110000
add nsw nuw 001?????, 00000001
sub nuw 0000????, 00000001
add self ????????
compare add 1111????, 00000001
exhaust udiv exact 0001????, 00000010
%x = and 10??0011, 11110000
%y = add %x, 00000001
```

Patterns are written most-significant bit first. Binary operands must have the
same nonzero width, except that the pure KnownBits shift APIs permit a separate
width for the shift amount.

## Scope

This program calls the pure operations in `llvm/Support/KnownBits.h`. It does
not construct LLVM IR or invoke the recursive, context-sensitive
`computeKnownBits(Value *)` analysis. The concrete evaluator used by `exhaust`
is separate and exists only to measure the precision of those LLVM results.
Structured JSON output could support a future web or Compiler Explorer UI.
