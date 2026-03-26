# Feedback Log (Ivan review 25-Mar-2026)

## 1) Benchmark docs mention "PR feedback"
Comment: [r2988653677](https://github.com/vcmi/vcmi/pull/7121#discussion_r2988653677)

Status: Done in `fixup! perf: add headless RMG benchmark CLI`.

What changed:
- Reworded benchmark docs to remove PR-internal wording.
- Kept the text self-contained and scenario-focused.

Ready response:
"Good point. I removed PR-specific wording and rewrote the section to explain
the scenario directly, so the docs stay clear when read out of PR context."

## 2) Use existing G-sized template
Comment: [r2988659081](https://github.com/vcmi/vcmi/pull/7121#discussion_r2988659081)

## 3) Use existing G-sized template (duplicate)
Comment: [r2988660729](https://github.com/vcmi/vcmi/pull/7121#discussion_r2988660729)

Status: Done in `fixup! perf: add headless RMG benchmark CLI`.

What changed:
- Default benchmark scenario now uses built-in `vcmi:Clash of Dragons`.
- Updated defaults to G+U-compatible size (`252x252x2`) and `expected-zones=18`.
- Updated docs to describe this built-in template scenario.

Ready response:
"Agreed. I switched defaults to an existing built-in G+U template scenario
(`vcmi:Clash of Dragons`) and updated both code and docs accordingly."

## 4) Why custom fuzz env logic was needed
Comment: [r2988675640](https://github.com/vcmi/vcmi/pull/7121#discussion_r2988675640)

Status: Done in `fixup! fuzz: add RMG reproducibility target`.

What changed:
- Removed custom working-directory probing and temporary dev-mode marker logic
  from `FuzzEnvironment.cpp`.
- Fuzzer init now uses straightforward `GameLibrary` initialization only.

Ready response:
"I removed the custom cwd/dev-marker setup and now use the standard library
initialization flow in the fuzzer environment, matching existing patterns."

## 5) Reuse `CBinaryReader` / `CMemoryStream`
Comment: [r2988714853](https://github.com/vcmi/vcmi/pull/7121#discussion_r2988714853)

Status: Done in `fixup! fuzz: add RMG reproducibility target`.

What changed:
- Removed the custom `ByteReader` class.
- Replaced fuzz input decoding with `CMemoryStream` + `CBinaryReader`.
- Kept robust short-input behavior by zero-padding to fixed spec size before
  decoding.

Ready response:
"Done. I dropped the custom byte reader and now decode fuzz inputs through
`CMemoryStream` + `CBinaryReader`, while preserving safe bounded behavior on
short inputs by decoding from a zero-padded fixed buffer."

## 6) Serialize map instead of manual fingerprint helper
Comment: [r2988813170](https://github.com/vcmi/vcmi/pull/7121#discussion_r2988813170)

Status: Done in `fixup! fuzz: add RMG reproducibility target`.

What changed:
- Removed the custom field-by-field map fingerprint code.
- Repro fuzzer now compares outputs of existing map save serialization
  (`CMapSaverJson`) across two generations of the same fuzz input.

Ready response:
"I removed the manual fingerprint method. The reproducibility check now compares
outputs from the existing map serialization path (`CMapSaverJson`) for two
same-input generations."

## 7) "What are these binaries?" (corpus files)
Comment: [r2988818111](https://github.com/vcmi/vcmi/pull/7121#discussion_r2988818111)

Status: Done in `fixup! fuzz: add RMG reproducibility target`.

What changed:
- Replaced binary committed seeds with human-readable `*.txt` seed files.
- Added `fuzz/corpus/vcmi-fuzz-rmg-repro/README.md` describing format and flow.
- Added conversion support inside existing fuzzer binary:
  - `--prepare-text-corpus=... --binary-corpus-out=...`
  - `--decode-artifact=... --text-out=...`
  - `--decode-corpus=... --text-out-dir=...`
- Updated `docs/developers/Fuzzing.md` with the new workflow.

Ready response:
"I switched curated corpus seeds to text and documented the format. I also added
text<->binary corpus conversion modes directly into `vcmi-fuzz-rmg-repro`, so
we don’t need a separate helper tool while still keeping libFuzzer runtime
corpus binary."

## Validation summary
- Remote build passed for:
  - `vcmi-rmg-bench`
  - `vcmitest`
  - `vcmi-fuzz-rmg-repro`
- Remote tests passed:
  - `RmgDeterminism.*`
- Fuzzer conversion modes validated remotely:
  - text corpus -> binary corpus
  - decode single artifact
  - decode corpus directory
- Short fuzz smoke run executed and produced a repro artifact; decode path works.

## Long fuzz run request result
- Attempted long run on remote host with prepared text corpus and
  `-max_total_time` configuration.
- The run does not reach long horizon because it quickly finds a reproducible
  abort input and exits with a crash artifact.
- Latest decoded crash seed:
  - `seed=16711681`
  - `singleThread=0`
  - `parallelism=134283008`
  - `creationDateTime=1742174175`
- This means the target is currently finding a reproducibility failure very
  early, so there is no stable 2-hour uninterrupted run yet.
