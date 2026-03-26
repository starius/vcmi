# Ivan Review (25-Mar-2026) for PR #7121

Source review: https://github.com/vcmi/vcmi/pull/7121#pullrequestreview-4007156568
Reviewer: `IvanSavenko`
Submitted at: `2026-03-25T15:06:52Z`

## Comments (all from this review)

1. File: `docs/developers/RmgBenchmark.md`
   URL: https://github.com/vcmi/vcmi/pull/7121#discussion_r2988653677
   Comment:
   > Which PR? 
   > Sure, you mean this PR, but not so clear for someone reading the docs a month from now.
   >
   > Better to do explain that scenario and drop reference to pr

2. File: `docs/developers/RmgBenchmark.md`
   URL: https://github.com/vcmi/vcmi/pull/7121#discussion_r2988659081
   Comment:
   > Can we use one of existing g-sized templates?

3. File: `docs/developers/RmgBenchmark.md`
   URL: https://github.com/vcmi/vcmi/pull/7121#discussion_r2988660729
   Comment:
   > Can we use one of existing g-sized templates?

4. File: `fuzz/FuzzEnvironment.cpp`
   URL: https://github.com/vcmi/vcmi/pull/7121#discussion_r2988675640
   Comment:
   > Why existing logic is not sufficient? For example tests also run fine

5. File: `fuzz/FuzzEnvironment.cpp`
   URL: https://github.com/vcmi/vcmi/pull/7121#discussion_r2988714853
   Comment:
   > We already have CBinaryReader / CMemoryStream classes. 
   > No need to add this massive amount of code when similar routines already exist

6. File: `fuzz/RmgReproFuzzer.cpp`
   URL: https://github.com/vcmi/vcmi/pull/7121#discussion_r2988813170
   Comment:
   > Why you even need this method? Just serialize map class itself 

7. File: `fuzz/corpus/vcmi-fuzz-rmg-repro/parallel.seed`
   URL: https://github.com/vcmi/vcmi/pull/7121#discussion_r2988818111
   Comment:
   > What are these binaries?

## Plan To Address

1. Benchmark docs: remove PR-internal wording and explain the scenario directly.
   - Replace text like "discussed in PR feedback" with a self-contained rationale.
   - Explain what workload the default is trying to stress (large-map RMG throughput and scheduler behavior), so docs stay clear over time.

2. Benchmark defaults/examples: move to an existing built-in G-size template.
   - Change default benchmark scenario from synthetic large dimensions to a reproducible built-in G+U-compatible template setup.
   - Keep the scenario headless and deterministic (fixed seed, timestamp), and document the exact template id.
   - Update examples in `RmgBenchmark.md` to match those defaults.

3. Fuzz environment init: reuse existing VCMI/test initialization patterns and remove extra cwd/dev-marker logic if not strictly needed.
   - Re-check if fuzz binary can initialize with the same filesystem/library init style already used by tests.
   - If yes, remove custom working-directory probing and temporary marker creation.
   - Keep only minimal initialization required for stable fuzzer startup.

4. Input decoding: replace custom `ByteReader` implementation with existing stream/reader primitives.
   - Use `CMemoryStream` + `CBinaryReader` for decoding fuzz bytes.
   - Keep bounded/fallback decoding behavior (on short input) at call sites, instead of maintaining a separate custom byte parser.
   - This reduces custom code and aligns with existing codebase utilities.

5. Repro comparison in fuzzer: serialize map using existing map serialization path.
   - Replace bespoke `serializeMapState` with serialization of `CMap` through existing serializer APIs.
   - Compare serialized outputs between two runs of the same input.
   - Preserve optional thread-invariance mode as a separate check.

6. Corpus seed files: document binary corpus format and purpose.
   - Add a short README in `fuzz/corpus/vcmi-fuzz-rmg-repro/` explaining that seeds are raw libFuzzer byte inputs.
   - Document field layout and provide hex dump examples for `single-thread.seed` and `parallel.seed`.
   - Keep files binary (libFuzzer-native), but make them understandable to reviewers.

7. Validation after changes (implementation phase later):
   - Build `vcmi-fuzz-rmg-repro` and run a short smoke fuzz session.
   - Run `RmgDeterminism.*` tests to ensure determinism checks remain green.
   - Run `vcmi-rmg-bench --list-templates` and one default benchmark run to verify docs/defaults match behavior.
