# RMG Benchmark

`vcmi-rmg-bench` is a headless command-line benchmark for random map generation.
It is meant for performance comparisons between commits, scheduler modes, and
thread counts.

## Build

Use the dedicated benchmark preset:

```sh
cmake --preset linux-gcc-bench
cmake --build --preset linux-gcc-bench --target vcmi-rmg-bench
```

Binary location:

```text
out/build/linux-gcc-bench/bin/vcmi-rmg-bench
```

## Defaults

The tool defaults to a heavy stress scenario intended for scheduler and
throughput checks:

- `--width 504`
- `--height 504`
- `--levels 4`
- `--expected-zones 200`

This scenario requires compatible templates in loaded data/mods. If none match,
the benchmark now exits with a clear error instead of crashing.

If template selection cannot enforce exactly 200 zones, the benchmark prints the
actual zone count for traceability.

## Validation Workflow

When using this benchmark for review-driven changes:

- Run builds and performance checks on the remote benchmark machine.
- If you amend/rebase commits, validate each changed commit in place.
- Re-run affected checks per commit (incremental build is fine):
  - `vcmi-rmg-bench --list-templates`
  - one benchmark run with the intended scenario
  - related tests when behavior changes (for example `RmgDeterminism.*`)

## Repro Fuzzer Corpus Workflow

For reproducibility fuzzing in this branch, use a split workflow:

- Human-facing seeds stay in text form.
- Runtime corpus for libFuzzer stays binary in a temporary directory.
- Conversion is handled by the existing fuzz binary flow (text to binary before
  run, binary artifacts back to text after run), so no separate helper tool is
  required.

## Usage

List available templates:

```sh
out/build/linux-gcc-bench/bin/vcmi-rmg-bench --list-templates
```

Run with defaults:

```sh
out/build/linux-gcc-bench/bin/vcmi-rmg-bench
```

Run with explicit template and output CSV:

```sh
out/build/linux-gcc-bench/bin/vcmi-rmg-bench \
  --template-id vcmi:Clash\ of\ Dragons \
  --threads 8 \
  --scheduler parallel \
  --warmup 2 \
  --runs 20 \
  --output-csv /tmp/rmg-bench.csv
```

## Key flags

- `--template-id <id>` or `--template-path <json>`
- `--width`, `--height`, `--levels`
- `--players`, `--human-players`, `--comp-only-players`
- `--water random|none|normal|islands`
- `--monsters random|weak|normal|strong`
- `--seed`, `--seed-step`, `--timestamp`
- `--scheduler single|parallel`
- `--threads`
- `--warmup`, `--runs`
- `--expected-zones`
- `--output-csv <path>`
