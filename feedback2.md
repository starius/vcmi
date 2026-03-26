I did the following changes:

1. Moved commit "mapping: tie zip entry time to map creation" from
   https://github.com/vcmi/vcmi/pull/7123.
   This keeps map save determinism in the same series as generation
   determinism, so identical generation inputs produce identical archive
   metadata too, not only identical map content.

2. Adjusted commit "fuzz: add RMG reproducibility target" to skip empty
   repro fuzzer inputs.
   Empty input does not decode into a meaningful generation spec and was
   creating noisy failures at startup. Returning early for `size == 0`
   keeps the fuzzer focused on real reproducibility mutations.

3. Updated commit "rmg: make map creation time injectable" to include the
   editor async generation call-site changes required by the new generator
   API.
   The generator now accepts an explicit creation timestamp, and editor/
   benchmark callers must pass the updated signature so deterministic time
   injection works end-to-end.

4. Updated commit "rmg: make scheduler and connections deterministic" to
   include stable parallel ready-wave scheduling and the corresponding
   determinism checks.
   Without a stable ready-wave order, parallel interleavings could reorder
   side effects between modificators. Folding the scheduler stabilization and
   parallel determinism assertions into this commit keeps the fix and its
   verification together. The change was needed to maintain map generation determinism even across different number of workers. I.e. now we get the same map when running with 2 workers or 16 workers. This affected performance slightly: now it takes 3.87% more time compared to baseline.
