# Fuzzing

This document describes how to build and run VCMI fuzz targets locally.

The fuzzing build is currently intended for Clang with libFuzzer and sanitizers.

## Build with CMake presets

Use the dedicated configure and build presets:

```sh
cmake --preset linux-clang-fuzz
cmake --build --preset linux-clang-fuzz
```

Fuzzer binaries are written to `out/build/linux-clang-fuzz/bin/`.

## Nix shell for fuzzing

If you use Nix, enter the fuzzing shell to get Clang + compiler-rt:

```sh
nix develop ./nix#fuzzing
```

The default shell still works for regular builds and tests.

## Running fuzzers

Each target is a standalone libFuzzer executable. A typical invocation is:

```sh
out/build/linux-clang-fuzz/bin/<fuzzer-name> \
  /tmp/<fuzzer-name>.bin \
  -max_total_time=120
```

Use `-runs=<N>` for deterministic smoke runs or `-max_total_time=<seconds>` for
time-bounded fuzzing.

The reproducibility fuzzer target currently available is:

```text
vcmi-fuzz-rmg-repro
```

## Text corpus workflow for `vcmi-fuzz-rmg-repro`

Curated seeds for this target are stored as text files in:

```text
fuzz/corpus/vcmi-fuzz-rmg-repro
```

Prepare temporary binary corpus before fuzzing:

```sh
out/build/linux-clang-fuzz/bin/vcmi-fuzz-rmg-repro \
  --prepare-text-corpus=fuzz/corpus/vcmi-fuzz-rmg-repro \
  --binary-corpus-out=/tmp/vcmi-fuzz-rmg-repro.bin
```

Run fuzzer:

```sh
out/build/linux-clang-fuzz/bin/vcmi-fuzz-rmg-repro \
  /tmp/vcmi-fuzz-rmg-repro.bin \
  -artifact_prefix=/tmp/vcmi-fuzz-rmg-repro.artifacts/ \
  -max_total_time=120
```

Decode a single crash/artifact back to text:

```sh
out/build/linux-clang-fuzz/bin/vcmi-fuzz-rmg-repro \
  --decode-artifact=/tmp/vcmi-fuzz-rmg-repro.artifacts/crash-... \
  --text-out=/tmp/vcmi-fuzz-rmg-repro.artifacts/crash.txt
```

Decode a whole binary corpus directory:

```sh
out/build/linux-clang-fuzz/bin/vcmi-fuzz-rmg-repro \
  --decode-corpus=/tmp/vcmi-fuzz-rmg-repro.bin \
  --text-out-dir=/tmp/vcmi-fuzz-rmg-repro.text
```
