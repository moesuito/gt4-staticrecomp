# EE name table from gt4-decomp (TASK-020, level 1)

2026-10-17. Read-only; no generated EE file, pin or runtime code is touched.

`docs/data/ee-names.csv` maps EE addresses to names, built by `scripts/ee_symbols.py`
from [gt4-decomp](https://github.com/gustavocanepa/gt4-decomp) at commit
`0b54ed5c563d15ff1ac0093b07d94c9b25cb98aa` (the same game code as our CORE, see
[DECOMP-IDENTITY](DECOMP-IDENTITY.md)). Regenerate with:

```
python scripts/ee_symbols.py --decomp <gt4-decomp checkout>
```

## Columns

| column | meaning |
|---|---|
| `address` | `0x` plus 8 hex digits, sorted ascending |
| `name` | decomp symbol (`Class__method`), or `Class::method` for Adhoc natives |
| `kind` | `func` or `data` |
| `origin` | `decomp` (RTTI, libraries, matched work), `gthd` (carried over from Gran Turismo HD, already applied in the decomp) or `adhoc` (script natives registered by the engine) |
| `quality` | `named`, or `placeholder` for `func_ADDR` and `Class__virtual_NN` slots |

One address can have several rows (aliases, `gthd` and `decomp` names for the same function).
Rows are unique per (address, name). 11,481 rows at this commit: 2,459 Adhoc natives, 4,648
named functions plus 3,712 placeholder virtual slots from decomp/gthd, and 662 data symbols.

## What this does not do

- It does not annotate `unit_XXXX.cpp`. Generated files are hash-pinned
  ([EE-PRODUCER](EE-PRODUCER.md)), so labelling them needs the new producer contract
  described under "Level 1" in [DECOMP-INTEGRATION](DECOMP-INTEGRATION.md). This table is the
  input for that step, nothing more.
- Unapplied low/medium-confidence GT HD guesses (`gthd_names.txt`, `applied = no`) are left out.
- Addresses were not cross-checked against our private ELF here; `tests/python/test_ee_symbols.py`
  only checks shape (sorted, unique, function addresses 4-aligned and inside the text segment).
