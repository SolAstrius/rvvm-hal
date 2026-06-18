# rmm-test

Regression harness for RVVM's RMM rounding mode (IEEE 754 `roundTiesToAway`),
the bug behind [LekKit/RVVM#204](https://github.com/LekKit/RVVM/issues/204).

Each vector runs an FP op under `frm = RMM` (set with `fsrmi 4`) using a
DYN-rounding instruction — matching how RVVM dispatches RMM (off the dynamic
`frm` CSR, not the static `rm` field) — and compares the result against an
expected value baked into `vectors.inc`.

## Run

```
nix shell nixpkgs#llvmPackages.bintools -c make   # from this dir
make run RVVM=/path/to/rvvm
```

Prints `RMM-RESULT pass=N fail=M total=T`, then powers off via syscon.

## Regenerating vectors

`vectors.inc` is produced by `gen_mpfr.c`, which uses MPFR as an independent
`roundTiesToAway` oracle (synthesized from directed rounding + exact-midpoint
comparison, since MPFR has no native ties-away mode). Ground truth covers all
five ops (add/sub/mul/div/sqrt) in f32 and f64, plus subnormal and
normal/subnormal-boundary cases.

```
nix shell nixpkgs#mpfr.dev nixpkgs#gmp.dev nixpkgs#mpfr nixpkgs#gmp -c \
  sh -c 'cc -O2 gen_mpfr.c -lmpfr -lgmp -o /tmp/gen_mpfr && /tmp/gen_mpfr > vectors.inc'
```
