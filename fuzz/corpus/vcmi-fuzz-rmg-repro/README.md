# `vcmi-fuzz-rmg-repro` Corpus

This corpus is stored in text form for reviewability.

Each `*.txt` file is a deterministic seed config:

- `seed=<int>`
- `singleThread=<0|1>`
- `parallelism=<int>`
- `creationDateTime=<unix timestamp>`

The fuzzer itself consumes binary corpus entries. Convert text seeds to a
temporary binary corpus before fuzzing:

```sh
out/build/linux-clang-fuzz/bin/vcmi-fuzz-rmg-repro \
  --prepare-text-corpus=fuzz/corpus/vcmi-fuzz-rmg-repro \
  --binary-corpus-out=/tmp/vcmi-fuzz-rmg-repro.bin
```

Decode crash/artifact files back to text:

```sh
out/build/linux-clang-fuzz/bin/vcmi-fuzz-rmg-repro \
  --decode-artifact=/tmp/vcmi-fuzz-rmg-repro.artifacts/crash-... \
  --text-out=/tmp/vcmi-fuzz-rmg-repro.artifacts/crash.txt
```
