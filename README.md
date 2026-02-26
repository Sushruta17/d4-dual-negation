# d4-dual-negation

A knowledge compilation tool that compiles propositional CNF formulas into **POG (Partitioned Operation Graph)** representations using a **dual-negation** strategy. Based on the [d4 compiler](https://github.com/crillab/d4) from Univ. Artois & CNRS.

---

## What is Dual-Negation?

Standard d-DNNF compilation recursively decomposes both branches of every decision. The **dual-negation** approach exploits the identity **φ ≡ ¬(¬φ)**: when clauses remain after a decision, instead of full recursive compilation, we **negate** the remaining formula and wrap it with a **NOT node**. This often produces smaller representations since the negated formula typically reduces to simple unit conjunctions.

---

## Building

### Prerequisites

- **g++** with C++11 support
- **Boost** (multiprecision, GMP backend)
- **GMP** library (`libgmpxx`, `libgmp`)
- **zlib**

### Compile

```bash
make -j8      # Standard build (with debug symbols)
```

### Clean

```bash
make clean
```

---

## Usage

### POG Compilation

```bash
./d4 -dDNNF <input.cnf>
```

### POG Compilation with Output File

```bash
./d4 -dDNNF <input.cnf> -out=<output.nnf>
```

### Example

```bash
# Create a test CNF: (x₁ ∨ x₂) ∧ (¬x₁ ∨ x₃ ∨ x₄)
echo "p cnf 4 2
1 2 0
-1 3 4 0" > test.cnf

# Compile and write output
./d4 -dDNNF test.cnf -out=result.nnf

# View the result
cat result.nnf
```

**Output:**
```
o 1 0
o 2 0
t 3 0
n 4 0
4 3 -3 -4 0
2 3 -1 2 0
2 4 1 0
1 2 0
```

---

## Output Format (`.nnf`)

The `.nnf` file uses a line-based encoding for the compiled DAG:

### Node Declarations

| Line Format | Node Type | Description |
|-------------|-----------|-------------|
| `o <id> 0` | **OR** | Deterministic OR (decision) node |
| `a <id> 0` | **AND** | Decomposable AND node (independent components) |
| `t <id> 0` | **TRUE** | Terminal true node |
| `f <id> 0` | **FALSE** | Terminal false node |
| `n <id> 0` | **NOT** | Negation node (POG extension) |

### Edge Lines

```
<parent_id> <child_id> <literal₁> <literal₂> ... 0
```

Each edge connects a parent node to a child, annotated with **unit literals** — the decision literal and any implied literals from Boolean Constraint Propagation (BCP).

**Literal encoding:** Positive integer = positive literal, negative integer = negated literal. For example, `-1 2` means x₁=false and x₂=true.

### Reading the Output

For the example `(x₁ ∨ x₂) ∧ (¬x₁ ∨ x₃ ∨ x₄)`:

```
o 2 0              ← Decision node on x₁
t 3 0              ← TRUE terminal
n 4 0              ← NOT node (dual-negation)
4 3 -3 -4 0        ← ¬x₃ ∧ ¬x₄ → TRUE
2 3 -1 2 0         ← x₁=false, x₂=true (BCP implied) → TRUE
2 4 1 0            ← x₁=true → NOT(¬x₃ ∧ ¬x₄) = (x₃ ∨ x₄)
```

**Interpretation:**
- When **x₁ = false**: BCP forces x₂ = true, all clauses satisfied → TRUE
- When **x₁ = true**: Clause 2 `(x₃ ∨ x₄)` remains → negated to `(¬x₃ ∧ ¬x₄)`, wrapped with NOT

---


## Input Format (DIMACS CNF)

Standard DIMACS CNF format:

```
p cnf <num_variables> <num_clauses>
<lit₁> <lit₂> ... 0
<lit₁> <lit₂> ... 0
...
```

- Variables are numbered `1` to `n`
- Positive literal: variable number (e.g., `3` for x₃)
- Negative literal: negated variable (e.g., `-3` for ¬x₃)
- Each clause ends with `0`

---


Original d4 compiler: Copyright (C) 2020 Univ. Artois & CNRS.
