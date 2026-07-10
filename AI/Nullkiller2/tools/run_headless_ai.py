#!/usr/bin/env python3
"""
Run VCMI headless AI games and summarize game outcome separately from
process exit status.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
MAP_LOADED_RE = re.compile(r"\bMap loaded!")
WINNER_RE = re.compile(r"\b([A-Za-z]+) player won\. Ending game\.")
LOSER_RE = re.compile(r"\b([A-Za-z]+) player lost\. Ending game\.")


@dataclass
class ConfigReplacement:
	file: Path
	old: str
	new: str


@dataclass
class GameOutcome:
	map_loaded: bool
	winner: str | None
	loser: str | None
	diagnostics: list[str]


@dataclass
class RunResult:
	index: int
	run_dir: Path
	command: list[str]
	exit_code: int | None
	timed_out: bool
	duration_seconds: float
	outcome: GameOutcome


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description="Run vcmiclient --headless AI games and summarize results."
	)
	parser.add_argument(
		"--client",
		default="./vcmiclient",
		help="Path to vcmiclient. Defaults to ./vcmiclient relative to --workdir.",
	)
	parser.add_argument(
		"--workdir",
		type=Path,
		default=None,
		help="Working directory for vcmiclient, usually the build bin directory.",
	)
	parser.add_argument(
		"--map",
		required=True,
		dest="map_path",
		help="Map path as accepted by --testmap, for example Maps/foo.vmap.",
	)
	parser.add_argument(
		"--ai",
		action="append",
		default=[],
		help="AI module to pass to vcmiclient. Repeat for multiple players.",
	)
	parser.add_argument(
		"--timeout",
		type=float,
		default=300.0,
		help="Seconds to wait for each run before terminating it.",
	)
	parser.add_argument(
		"--repetitions",
		type=int,
		default=1,
		help="Number of independent runs to execute.",
	)
	parser.add_argument(
		"--output-dir",
		type=Path,
		default=None,
		help="Directory for raw logs and summaries. Defaults to a timestamped directory.",
	)
	parser.add_argument(
		"--config-replace",
		nargs=3,
		action="append",
		default=[],
		metavar=("FILE", "OLD", "NEW"),
		help=(
			"Temporarily replace literal OLD with NEW in FILE for all runs. "
			"May be repeated. Original files are restored before exit."
		),
	)
	parser.add_argument(
		"--extra-arg",
		action="append",
		default=[],
		help="Extra argument passed through to vcmiclient. Repeat for each argument.",
	)
	parser.add_argument(
		"--require-winner",
		action="store_true",
		help="Return failure if no winner/loser ending line is found.",
	)
	parser.add_argument(
		"--expect-winner",
		help="Return failure if the parsed winner is not this player color.",
	)
	parser.add_argument(
		"--require-clean-exit",
		action="store_true",
		help="Return failure if vcmiclient exits with a nonzero code.",
	)

	args = parser.parse_args()

	if args.repetitions <= 0:
		parser.error("--repetitions must be positive")
	if args.timeout <= 0:
		parser.error("--timeout must be positive")
	if not args.ai:
		args.ai = ["Nullkiller2", "Nullkiller2"]
	if args.expect_winner:
		args.expect_winner = args.expect_winner.lower()

	return args


def default_output_dir() -> Path:
	timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
	return Path("headless-ai-runs") / timestamp


def resolve_config_replacements(args: argparse.Namespace) -> list[ConfigReplacement]:
	base = args.workdir or Path.cwd()
	replacements: list[ConfigReplacement] = []

	for raw_file, old, new in args.config_replace:
		file = Path(raw_file)
		if not file.is_absolute():
			file = base / file
		replacements.append(ConfigReplacement(file.resolve(), old, new))

	return replacements


def apply_config_replacements(
	replacements: list[ConfigReplacement],
	originals: dict[Path, bytes],
) -> None:
	for replacement in replacements:
		if replacement.file not in originals:
			originals[replacement.file] = replacement.file.read_bytes()

		text = replacement.file.read_text()
		if replacement.old not in text:
			raise RuntimeError(
				f"Replacement text not found in {replacement.file}: {replacement.old!r}"
			)
		replacement.file.write_text(text.replace(replacement.old, replacement.new))


def restore_config_files(originals: dict[Path, bytes]) -> None:
	for file, content in originals.items():
		file.write_bytes(content)


def build_command(args: argparse.Namespace, run_dir: Path) -> list[str]:
	command = [
		args.client,
		"--headless",
		"--testmap",
		args.map_path,
	]

	for ai_name in args.ai:
		command.extend(["--ai", ai_name])

	command.extend(["--logLocation", str(run_dir.resolve())])
	command.extend(args.extra_arg)
	return command


def terminate_process(proc: subprocess.Popen[bytes]) -> None:
	if os.name == "nt":
		proc.terminate()
	else:
		os.killpg(proc.pid, signal.SIGTERM)

	try:
		proc.wait(timeout=10)
	except subprocess.TimeoutExpired:
		if os.name == "nt":
			proc.kill()
		else:
			os.killpg(proc.pid, signal.SIGKILL)
		proc.wait()


def run_once(args: argparse.Namespace, output_dir: Path, index: int) -> RunResult:
	run_dir = output_dir / f"run-{index:03d}"
	run_dir.mkdir(parents=True, exist_ok=False)

	command = build_command(args, run_dir)
	(run_dir / "command.txt").write_text(shlex.join(command) + "\n")

	start = time.monotonic()
	timed_out = False
	exit_code: int | None

	with (run_dir / "stdout.txt").open("wb") as stdout:
		proc = subprocess.Popen(
			command,
			cwd=args.workdir,
			stdout=stdout,
			stderr=subprocess.STDOUT,
			start_new_session=(os.name != "nt"),
		)

		try:
			exit_code = proc.wait(timeout=args.timeout)
		except subprocess.TimeoutExpired:
			timed_out = True
			terminate_process(proc)
			exit_code = proc.returncode
			stdout.write(f"\nTimed out after {args.timeout} seconds.\n".encode())

	duration = time.monotonic() - start
	outcome = parse_outcome(run_dir)

	result = RunResult(
		index=index,
		run_dir=run_dir,
		command=command,
		exit_code=exit_code,
		timed_out=timed_out,
		duration_seconds=duration,
		outcome=outcome,
	)

	write_run_summary(result)
	return result


def iter_log_lines(run_dir: Path):
	log_files = [run_dir / "stdout.txt"]

	for log_file in log_files:
		if not log_file.exists():
			continue

		with log_file.open(errors="replace") as handle:
			for raw_line in handle:
				yield ANSI_RE.sub("", raw_line).strip()


def parse_outcome(run_dir: Path) -> GameOutcome:
	map_loaded = False
	winner = None
	loser = None
	diagnostics: list[str] = []

	for line in iter_log_lines(run_dir):
		if MAP_LOADED_RE.search(line):
			map_loaded = True

		winner_match = WINNER_RE.search(line)
		if winner_match:
			winner = winner_match.group(1)

		loser_match = LOSER_RE.search(line)
		if loser_match:
			loser = loser_match.group(1)

		if "Disaster" in line or "Reason:" in line:
			diagnostics.append(line)

	return GameOutcome(map_loaded, winner, loser, diagnostics[-20:])


def result_to_dict(result: RunResult) -> dict:
	process_status = get_process_status(result.exit_code)

	return {
		"index": result.index,
		"runDir": str(result.run_dir),
		"command": result.command,
		"returnCode": result.exit_code,
		"exitCode": process_status["exitCode"],
		"signal": process_status["signal"],
		"timedOut": result.timed_out,
		"durationSeconds": round(result.duration_seconds, 3),
		"mapLoaded": result.outcome.map_loaded,
		"winner": result.outcome.winner,
		"loser": result.outcome.loser,
		"diagnostics": result.outcome.diagnostics,
	}


def write_run_summary(result: RunResult) -> None:
	(result.run_dir / "summary.json").write_text(
		json.dumps(result_to_dict(result), indent=2) + "\n"
	)


def write_overall_summary(output_dir: Path, results: list[RunResult]) -> None:
	(output_dir / "summary.json").write_text(
		json.dumps([result_to_dict(result) for result in results], indent=2) + "\n"
	)


def print_summary(results: list[RunResult]) -> None:
	print()
	print("Run summary:")
	for result in results:
		status = "timeout" if result.timed_out else format_process_status(result.exit_code)
		winner = result.outcome.winner or "-"
		loser = result.outcome.loser or "-"
		print(
			f"run={result.index:03d} "
			f"exit={status} "
			f"loaded={str(result.outcome.map_loaded).lower()} "
			f"winner={winner} "
			f"loser={loser} "
			f"duration={result.duration_seconds:.1f}s "
			f"logs={result.run_dir}"
		)


def get_process_status(return_code: int | None) -> dict[str, int | str | None]:
	if return_code is None:
		return {"exitCode": None, "signal": None}

	if return_code >= 0:
		return {"exitCode": return_code, "signal": None}

	signal_number = -return_code
	try:
		signal_name = signal.Signals(signal_number).name
	except ValueError:
		signal_name = f"SIG{signal_number}"

	return {"exitCode": 128 + signal_number, "signal": signal_name}


def format_process_status(return_code: int | None) -> str:
	status = get_process_status(return_code)
	if status["signal"]:
		return f"{status['exitCode']}({status['signal']})"

	return str(status["exitCode"])


def has_failed(args: argparse.Namespace, results: list[RunResult]) -> bool:
	for result in results:
		if result.timed_out or not result.outcome.map_loaded:
			return True

		if args.require_clean_exit and result.exit_code != 0:
			return True

		if args.require_winner and not (result.outcome.winner or result.outcome.loser):
			return True

		if args.expect_winner and (result.outcome.winner or "").lower() != args.expect_winner:
			return True

	return False


def main() -> int:
	args = parse_args()
	output_dir = args.output_dir or default_output_dir()
	output_dir.mkdir(parents=True, exist_ok=True)

	replacements = resolve_config_replacements(args)
	originals: dict[Path, bytes] = {}
	results: list[RunResult] = []
	error: Exception | None = None

	try:
		apply_config_replacements(replacements, originals)
		for index in range(1, args.repetitions + 1):
			results.append(run_once(args, output_dir, index))
	except Exception as exc:
		error = exc
	finally:
		restore_config_files(originals)

	if results:
		write_overall_summary(output_dir, results)
		print_summary(results)
		print(f"\nRaw logs preserved in: {output_dir}")

	if error:
		print(f"error: {error}", file=sys.stderr)
		return 2

	return 1 if has_failed(args, results) else 0


if __name__ == "__main__":
	sys.exit(main())
