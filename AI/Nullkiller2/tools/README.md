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
