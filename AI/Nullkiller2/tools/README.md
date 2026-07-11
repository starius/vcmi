# Nullkiller Headless AI Harness

`run_headless_ai.py` runs `vcmiclient --headless --testmap` for repeatable
AI-vs-AI smoke checks. It keeps raw logs for every run and reports the game
outcome separately from the client process exit code.

This is developer tooling, not a CI contract. Some headless runs can reach a
valid game result and still exit nonzero during shutdown. Use the parsed
outcome and the process status together.

## Basic Usage

From the repository root, after building the client:

```sh
python3 AI/Nullkiller2/tools/run_headless_ai.py \
  --workdir build/bin \
  --client ./vcmiclient \
  --map Maps/agent-fixtures/dd-victory-red.vmap \
  --ai Nullkiller2 \
  --ai Nullkiller2 \
  --timeout 75 \
  --repetitions 3 \
  --require-winner
```

Each repetition gets its own directory containing:

- `command.txt`
- `stdout.txt`
- VCMI log files written by `--logLocation`
- `summary.json`

The parent output directory also receives an aggregate `summary.json`.
The JSON stores both Python's raw process return code and the shell-style
exit code. Signal exits are printed like `134(SIGABRT)`.

## Temporary Config Overrides

Use `--config-replace FILE OLD NEW` to make literal temporary replacements
before the runs. Original files are restored before the script exits, including
on failures.

Enable object graph for one harness invocation:

```sh
python3 AI/Nullkiller2/tools/run_headless_ai.py \
  --workdir build/bin \
  --map Maps/agent-fixtures/dd-victory-red.vmap \
  --config-replace \
    config/ai/nk2ai/nk2ai-settings.json \
    '"allowObjectGraph": false' \
    '"allowObjectGraph": true'
```

Enable guarded Dimension Door landings for one harness invocation:

```sh
python3 AI/Nullkiller2/tools/run_headless_ai.py \
  --workdir build/bin \
  --map Maps/agent-fixtures/dd-guarded-red.vmap \
  --config-replace \
    config/gameConfig.json \
    '"dimensionDoorTriggersGuards" : false' \
    '"dimensionDoorTriggersGuards" : true'
```

## Exit Behavior

By default, the script fails when a run times out or the map never loads. It
does not fail solely because `vcmiclient` exits nonzero after producing a game
outcome.

Additional checks:

- `--require-winner` fails if no winner or loser ending line is found.
- `--expect-winner Red` fails if the parsed winner is not Red.
- `--require-clean-exit` fails if the client process exit code is nonzero.

## Battle Predictor A/B

`compare_battle_predictors.py` compares `Nullkiller2` with a configured
candidate such as `Nullkiller2Ratio`, `Nullkiller2V2`, `Nullkiller2V3`, or
`Nullkiller2V3Simulation`.
The default `--comparison-mode color-swap` is for true competitive maps: it
runs each sample twice and swaps Red/Blue to control for color advantage.

For scripted AI-test maps where Red is the tested role, use
`--comparison-mode red-role`. That mode runs legacy-as-Red and candidate-as-Red
against the same Blue opponent and sign-tests only samples where one Red player
wins and the other does not.

For generated-map experiments, use `--random-map` instead of `--map`. The
sample seed is passed both as the game seed and as `--randommap-seed`, so the
legacy and candidate runs in a pair use the same generated map:

```sh
python3 AI/Nullkiller2/tools/compare_battle_predictors.py \
  --comparison-mode color-swap \
  --random-map \
  --randommap-size S \
  --randommap-levels 2 \
  --randommap-water none \
  --randommap-players 2 \
  --samples 50 \
  --legacy-ai Nullkiller2 \
  --candidate-ai Nullkiller2V3 \
  --testdays 28 \
  --adjudicate-testdays
```

To test V3 with runtime battle simulation enabled, keep `--legacy-ai
Nullkiller2` and use `--candidate-ai Nullkiller2V3Simulation`. This alias uses
V3 with 15 runtime samples and planner ratio `1.0`, leaving the default
`Nullkiller2` and `Nullkiller2V3` settings unchanged:

```sh
python3 AI/Nullkiller2/tools/compare_battle_predictors.py \
  --comparison-mode color-swap \
  --random-map \
  --randommap-size S \
  --randommap-levels 2 \
  --randommap-water none \
  --randommap-players 2 \
  --samples 125 \
  --legacy-ai Nullkiller2 \
  --candidate-ai Nullkiller2V3Simulation \
  --testdays 28 \
  --adjudicate-testdays \
  --require-runtime-simulation candidate \
  --min-runtime-simulation-planning-decisions 1
```

Summary files include `runtimeBattleSimulation` totals by model. The
`--require-runtime-simulation candidate` guard makes the run fail if valid
candidate games do not have simulation requests or if fewer than 90% of those
requests complete. Add `--min-runtime-simulation-planning-decisions` when the
run should also prove that planner-side simulation produced completed accepted
or rejected decisions. Add `--min-runtime-simulation-planning-vetoes` or
`--min-runtime-simulation-planning-rescues` when the run should prove that
planner-side simulation specifically rejected static-safe targets or accepted
static-unsafe targets.
The planning ratio replacement only affects offensive planning when V3 runtime
simulation is enabled; it lets more candidate attacks reach the final simulator
gate without changing defensive threat checks.
When planner-side simulation is active, summaries also include
`planningAccepted`, `planningRejected`, and `planningIncomplete` counters under
`runtimeBattleSimulation`. These count same-turn, current-army capture targets
that were rechecked before movement planning: accepted targets got a complete
safe simulation verdict, rejected targets got a complete unsafe simulation
verdict, and incomplete targets could not get a complete simulation verdict.
The split fields `planningRejectedStaticSafe` and
`planningAcceptedStaticUnsafe` are the key proof counters for static false-safe
vetoes and static false-unsafe rescues.
Runtime and planner simulation currently treat a battle as safe only if the
attacker wins every requested sample. This matches the best observed town/siege
offline safety policy and avoids accepting 19/20-style near misses as safe.

Latest large run note: a 250-pair generated-map A/B with 15 runtime samples and
planning ratio `1.0` finished neutral: candidate 251-249 by games, 38
candidate sweeps vs 37 legacy sweeps, 175 splits, and two-sided sign-test p
`1.0`. Runtime evidence was active, with 256/257 requests complete. This proves
the harness and runtime gate are working at scale, but not that the current gate
improves end-to-end Nullkiller strength.

With `--adjudicate-testdays`, games that reach the completed-day limit without
a standard winner are scored deterministically from the run-local
`statistics.csv`. Full standard victories still take precedence when they occur.
