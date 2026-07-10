#!/usr/bin/env python3
"""
Run paired headless AI games comparing legacy Nullkiller battle prediction with
an adjusted predictor variant.

Each sample runs twice with the same map and seed:

1. Red = legacy, Blue = candidate
2. Red = candidate, Blue = legacy

This controls for color advantage. The statistical summary uses the color-swapped
pair as the unit of evidence.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
MAP_LOADED_RE = re.compile(r"\bMap loaded!")
WINNER_RE = re.compile(r"\b([A-Za-z]+) player won\. Ending game\.")
LOSER_RE = re.compile(r"\b([A-Za-z]+) player lost\. Ending game\.")
TEST_DAY_LIMIT_RE = re.compile(r"\bReached test day limit\b")

ADJUDICATION_FIELDS = [
	"statusRank",
	"NumberTowns",
	"ArmyStrength",
	"NumberHeroes",
	"Income",
	"TotalExperience",
	"MaxHeroLevel",
	"NumberArtifacts",
	"NumberDwellings",
	"NumWinBattlesPlayer",
	"NumWinBattlesNeutral",
	"MapExploredRatio",
	"MovementPointsUsed",
	"Score",
]

COLOR_ALIASES = {
	"red": "Red",
	"blue": "Blue",
	"tan": "Tan",
	"green": "Green",
	"orange": "Orange",
	"purple": "Purple",
	"teal": "Teal",
	"pink": "Pink",
}


@dataclass(frozen=True)
class GameTask:
	sample: int
	run: int
	seed: int
	map_path: str
	direction: str
	red_ai: str
	blue_ai: str
	red_model: str
	blue_model: str
	server_port: int
	run_dir: Path


@dataclass
class GameResult:
	task: GameTask
	command: list[str]
	exit_code: int | None
	timed_out: bool
	duration_seconds: float
	map_loaded: bool
	winner_color: str | None
	loser_color: str | None
	winner_model: str | None
	test_day_limited: bool
	adjudicated: bool
	adjudication_day: int | None
	adjudication_reason: str | None
	adjudication_vectors: dict[str, list[float | int]] | None
	diagnostics: list[str]


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description="Run paired headless games comparing Nullkiller predictor variants."
	)
	parser.add_argument("--client", default="./vcmiclient", help="Path to vcmiclient relative to --workdir unless absolute.")
	parser.add_argument("--workdir", type=Path, default=None, help="Working directory for vcmiclient, usually the build bin directory.")
	parser.add_argument("--map", dest="maps", action="append", help="Map path accepted by --testmap. Repeat to cycle maps.")
	parser.add_argument("--random-map", action="store_true", help="Use --testrandommap instead of --testmap.")
	parser.add_argument("--randommap-size", default="S", help="Generated map size for --random-map.")
	parser.add_argument("--randommap-levels", type=int, default=2, help="Generated map level count for --random-map.")
	parser.add_argument("--randommap-players", type=int, default=2, help="Generated human-or-AI player count for --random-map.")
	parser.add_argument("--randommap-teams", type=int, default=0, help="Generated map team count; 0 means free-for-all.")
	parser.add_argument("--randommap-comp-only-players", type=int, default=0, help="Generated computer-only player count.")
	parser.add_argument("--randommap-comp-only-teams", type=int, default=0, help="Generated computer-only team count.")
	parser.add_argument("--randommap-water", default="none", choices=("none", "normal", "islands", "random"), help="Generated map water content.")
	parser.add_argument("--randommap-monsters", default="normal", choices=("weak", "normal", "strong", "random"), help="Generated map monster strength.")
	parser.add_argument("--randommap-template", default=None, help="Generated map template id.")
	parser.add_argument("--testdays", type=int, default=0, help="Optional completed-day limit passed to vcmiclient.")
	parser.add_argument("--adjudicate-testdays", action="store_true", help="If no player wins by --testdays, pick a deterministic winner from run-local statistics.csv.")
	parser.add_argument("--legacy-ai", default="Nullkiller2", help="AI name for the baseline player.")
	parser.add_argument("--candidate-ai", default="Nullkiller2Ratio", help="AI name for the adjusted predictor player.")
	parser.add_argument("--opponent-ai", default="Nullkiller2", help="Fixed Blue opponent for --comparison-mode red-role.")
	parser.add_argument(
		"--comparison-mode",
		choices=("color-swap", "red-role"),
		default="color-swap",
		help="color-swap compares two competitive players; red-role compares both variants in the same Red role.",
	)
	parser.add_argument("--samples", type=int, default=30, help="Number of paired samples to run.")
	parser.add_argument("--seed-start", type=int, default=100000, help="First deterministic server seed.")
	parser.add_argument("--seed-step", type=int, default=1, help="Increment between sample seeds.")
	parser.add_argument("--timeout", type=float, default=300.0, help="Seconds before terminating one game run.")
	parser.add_argument("--jobs", type=int, default=1, help="Number of game processes to run in parallel.")
	parser.add_argument("--base-server-port", type=int, default=43030, help="First local server port assigned to game processes.")
	parser.add_argument("--output-dir", type=Path, default=None, help="Output directory. Defaults to a timestamped directory.")
	parser.add_argument("--extra-arg", action="append", default=[], help="Extra argument passed to vcmiclient. Repeat per argument.")
	parser.add_argument("--keep-engine-logs", action="store_true", help="Keep VCMI log files in each run directory. Stdout and summaries are always kept.")
	parser.add_argument("--require-clean-exit", action="store_true", help="Mark nonzero vcmiclient exits as failed games.")
	args = parser.parse_args()

	if args.samples <= 0:
		parser.error("--samples must be positive")
	if not args.random_map and not args.maps:
		parser.error("--map is required unless --random-map is used")
	if args.random_map and args.maps:
		parser.error("--map cannot be combined with --random-map")
	if args.randommap_levels <= 0:
		parser.error("--randommap-levels must be positive")
	if args.randommap_players <= 0:
		parser.error("--randommap-players must be positive")
	if args.seed_step <= 0:
		parser.error("--seed-step must be positive")
	if args.timeout <= 0:
		parser.error("--timeout must be positive")
	if args.adjudicate_testdays and args.testdays <= 0:
		parser.error("--adjudicate-testdays requires --testdays")
	if args.jobs <= 0:
		parser.error("--jobs must be positive")
	if args.base_server_port <= 0 or args.base_server_port > 65535:
		parser.error("--base-server-port must be a valid TCP port")
	if args.base_server_port + args.samples * 2 > 65535:
		parser.error("--base-server-port is too high for the requested number of samples")

	return args


def default_output_dir() -> Path:
	timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
	return Path("predictor-ab-runs") / timestamp


def canonical_color(value: str | None) -> str | None:
	if not value:
		return None
	return COLOR_ALIASES.get(value.lower(), value)


def opposite_two_player_color(color: str | None) -> str | None:
	if color == "Red":
		return "Blue"
	if color == "Blue":
		return "Red"
	return None


def model_for_color(task: GameTask, color: str | None) -> str | None:
	if color == "Red":
		return task.red_model
	if color == "Blue":
		return task.blue_model
	return None


def build_tasks(args: argparse.Namespace, output_dir: Path) -> list[GameTask]:
	tasks: list[GameTask] = []
	task_index = 0

	for sample in range(1, args.samples + 1):
		seed = args.seed_start + (sample - 1) * args.seed_step
		map_path = "random-map" if args.random_map else args.maps[(sample - 1) % len(args.maps)]

		for run, direction in enumerate(("legacy-red", "candidate-red"), start=1):
			if args.comparison_mode == "red-role":
				if direction == "legacy-red":
					red_ai = args.legacy_ai
					red_model = "legacy"
				else:
					red_ai = args.candidate_ai
					red_model = "candidate"
				blue_ai = args.opponent_ai
				blue_model = "opponent"
			else:
				if direction == "legacy-red":
					red_ai = args.legacy_ai
					blue_ai = args.candidate_ai
					red_model = "legacy"
					blue_model = "candidate"
				else:
					red_ai = args.candidate_ai
					blue_ai = args.legacy_ai
					red_model = "candidate"
					blue_model = "legacy"

			task_index += 1
			run_dir = output_dir / f"sample-{sample:04d}" / direction
			tasks.append(
				GameTask(
					sample=sample,
					run=run,
					seed=seed,
					map_path=map_path,
					direction=direction,
					red_ai=red_ai,
					blue_ai=blue_ai,
					red_model=red_model,
					blue_model=blue_model,
					server_port=args.base_server_port + task_index,
					run_dir=run_dir,
				)
			)

	return tasks


def build_command(args: argparse.Namespace, task: GameTask) -> list[str]:
	command = [
		args.client,
		"--headless",
	]
	if args.random_map:
		command.extend(
			[
				"--testrandommap",
				"--randommap-seed",
				str(task.seed),
				"--randommap-size",
				args.randommap_size,
				"--randommap-levels",
				str(args.randommap_levels),
				"--randommap-players",
				str(args.randommap_players),
				"--randommap-teams",
				str(args.randommap_teams),
				"--randommap-comp-only-players",
				str(args.randommap_comp_only_players),
				"--randommap-comp-only-teams",
				str(args.randommap_comp_only_teams),
				"--randommap-water",
				args.randommap_water,
				"--randommap-monsters",
				args.randommap_monsters,
			]
		)
		if args.randommap_template:
			command.extend(["--randommap-template", args.randommap_template])
	else:
		command.extend(["--testmap", task.map_path])

	command.extend(
		[
			"--seed",
			str(task.seed),
			"--serverport",
			str(task.server_port),
			"--ai",
			task.red_ai,
			"--ai",
			task.blue_ai,
			"--logLocation",
			str(task.run_dir.resolve()),
		]
	)
	if args.testdays:
		command.extend(["--testdays", str(args.testdays)])
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


def iter_log_lines(run_dir: Path):
	log_files = [run_dir / "stdout.txt"]

	for log_file in log_files:
		if not log_file.exists():
			continue

		with log_file.open(errors="replace") as handle:
			for raw_line in handle:
				yield ANSI_RE.sub("", raw_line).strip()


def parse_run_logs(task: GameTask) -> tuple[bool, str | None, str | None, bool, list[str]]:
	map_loaded = False
	winner = None
	loser = None
	test_day_limited = False
	diagnostics: list[str] = []

	for line in iter_log_lines(task.run_dir):
		if MAP_LOADED_RE.search(line):
			map_loaded = True
		if TEST_DAY_LIMIT_RE.search(line):
			test_day_limited = True

		winner_match = WINNER_RE.search(line)
		if winner_match:
			winner = canonical_color(winner_match.group(1))

		loser_match = LOSER_RE.search(line)
		if loser_match:
			loser = canonical_color(loser_match.group(1))

		if "Disaster" in line or "Reason:" in line:
			diagnostics.append(line)

	if winner is None and loser is not None:
		winner = opposite_two_player_color(loser)

	return map_loaded, winner, loser, test_day_limited, diagnostics[-20:]


def parse_stat_number(row: dict[str, str], field: str) -> float | int:
	value = (row.get(field) or "").strip()
	if not value:
		return 0
	try:
		if "." in value:
			return float(value)
		return int(value)
	except ValueError:
		return 0


def adjudication_vector(row: dict[str, str]) -> list[float | int]:
	status = int(parse_stat_number(row, "Status"))
	status_rank = {
		2: 1,   # WINNER
		0: 0,   # INGAME
		1: -1,  # LOSER
	}.get(status, -2)

	return [
		status_rank,
		parse_stat_number(row, "NumberTowns"),
		parse_stat_number(row, "ArmyStrength"),
		parse_stat_number(row, "NumberHeroes"),
		parse_stat_number(row, "Income"),
		parse_stat_number(row, "TotalExperience"),
		parse_stat_number(row, "MaxHeroLevel"),
		parse_stat_number(row, "NumberArtifacts"),
		parse_stat_number(row, "NumberDwellings"),
		parse_stat_number(row, "NumWinBattlesPlayer"),
		parse_stat_number(row, "NumWinBattlesNeutral"),
		parse_stat_number(row, "MapExploredRatio"),
		parse_stat_number(row, "MovementPointsUsed"),
		parse_stat_number(row, "Score"),
	]


def adjudicate_from_statistics(task: GameTask) -> tuple[dict | None, list[str]]:
	stats_path = task.run_dir / "statistics.csv"
	if not stats_path.exists():
		return None, [f"Missing adjudication statistics: {stats_path}"]

	latest_rows: dict[str, dict[str, str]] = {}
	with stats_path.open(encoding="utf-8", errors="replace", newline="") as handle:
		reader = csv.DictReader(handle, delimiter=";")
		for row in reader:
			color = canonical_color(row.get("Player"))
			if color not in ("Red", "Blue"):
				continue

			day = int(parse_stat_number(row, "Day"))
			current = latest_rows.get(color)
			if current is None or day >= int(parse_stat_number(current, "Day")):
				latest_rows[color] = row

	if "Red" not in latest_rows or "Blue" not in latest_rows:
		return None, ["Adjudication statistics do not contain both Red and Blue rows"]

	red_vector = adjudication_vector(latest_rows["Red"])
	blue_vector = adjudication_vector(latest_rows["Blue"])
	if red_vector == blue_vector:
		return None, ["Adjudication statistics are exactly tied"]

	winner_color = "Red" if red_vector > blue_vector else "Blue"
	winner_vector = red_vector if winner_color == "Red" else blue_vector
	loser_vector = blue_vector if winner_color == "Red" else red_vector
	day = int(parse_stat_number(latest_rows[winner_color], "Day"))
	reason = "statistics"
	for field, winner_value, loser_value in zip(ADJUDICATION_FIELDS, winner_vector, loser_vector):
		if winner_value != loser_value:
			reason = f"{field}: {winner_color} {winner_value} > {opposite_two_player_color(winner_color)} {loser_value}"
			break

	return {
		"winnerColor": winner_color,
		"day": day,
		"reason": reason,
		"vectors": {
			"Red": red_vector,
			"Blue": blue_vector,
		},
	}, []


def run_game(args: argparse.Namespace, task: GameTask) -> GameResult:
	task.run_dir.mkdir(parents=True, exist_ok=False)
	command = build_command(args, task)
	(task.run_dir / "command.txt").write_text(shlex.join(command) + "\n")

	start = time.monotonic()
	timed_out = False
	exit_code: int | None

	with (task.run_dir / "stdout.txt").open("wb") as stdout:
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
	map_loaded, winner_color, loser_color, test_day_limited, diagnostics = parse_run_logs(task)
	adjudicated = False
	adjudication_day = None
	adjudication_reason = None
	adjudication_vectors = None
	if (
		args.adjudicate_testdays
		and winner_color is None
		and test_day_limited
		and not timed_out
	):
		adjudication, adjudication_diagnostics = adjudicate_from_statistics(task)
		diagnostics.extend(adjudication_diagnostics)
		if adjudication:
			winner_color = adjudication["winnerColor"]
			loser_color = opposite_two_player_color(winner_color)
			adjudicated = True
			adjudication_day = adjudication["day"]
			adjudication_reason = adjudication["reason"]
			adjudication_vectors = adjudication["vectors"]

	winner_model = model_for_color(task, winner_color)
	if not args.keep_engine_logs:
		for log_file in task.run_dir.glob("*_log.txt"):
			log_file.unlink(missing_ok=True)

	result = GameResult(
		task=task,
		command=command,
		exit_code=exit_code,
		timed_out=timed_out,
		duration_seconds=duration,
		map_loaded=map_loaded,
		winner_color=winner_color,
		loser_color=loser_color,
		winner_model=winner_model,
		test_day_limited=test_day_limited,
		adjudicated=adjudicated,
		adjudication_day=adjudication_day,
		adjudication_reason=adjudication_reason,
		adjudication_vectors=adjudication_vectors,
		diagnostics=diagnostics,
	)
	write_run_summary(result)
	return result


def process_status(return_code: int | None) -> dict[str, int | str | None]:
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


def result_to_dict(result: GameResult) -> dict:
	status = process_status(result.exit_code)
	return {
		"sample": result.task.sample,
		"run": result.task.run,
		"seed": result.task.seed,
		"map": result.task.map_path,
		"direction": result.task.direction,
		"redAI": result.task.red_ai,
		"blueAI": result.task.blue_ai,
		"redModel": result.task.red_model,
		"blueModel": result.task.blue_model,
		"serverPort": result.task.server_port,
		"runDir": str(result.task.run_dir),
		"command": result.command,
		"returnCode": result.exit_code,
		"exitCode": status["exitCode"],
		"signal": status["signal"],
		"timedOut": result.timed_out,
		"durationSeconds": round(result.duration_seconds, 3),
		"mapLoaded": result.map_loaded,
		"winnerColor": result.winner_color,
		"loserColor": result.loser_color,
		"winnerModel": result.winner_model,
		"testDayLimited": result.test_day_limited,
		"adjudicated": result.adjudicated,
		"adjudicationDay": result.adjudication_day,
		"adjudicationReason": result.adjudication_reason,
		"adjudicationFields": ADJUDICATION_FIELDS if result.adjudication_vectors else None,
		"adjudicationVectors": result.adjudication_vectors,
		"diagnostics": result.diagnostics,
	}


def write_run_summary(result: GameResult) -> None:
	(result.task.run_dir / "summary.json").write_text(
		json.dumps(result_to_dict(result), indent=2) + "\n"
	)


def is_valid_result(args: argparse.Namespace, result: GameResult) -> bool:
	valid_models = {"legacy", "candidate"}
	if args.comparison_mode == "red-role":
		valid_models.add("opponent")
	if result.timed_out or not result.map_loaded or result.winner_model not in valid_models:
		return False
	if args.require_clean_exit and result.exit_code != 0:
		return False
	return True


def active_model_won(result: GameResult) -> bool:
	return result.winner_model == result.task.red_model


def wilson_interval(successes: int, total: int, z: float = 1.959963984540054) -> tuple[float | None, float | None]:
	if total == 0:
		return None, None

	p = successes / total
	denominator = 1 + z * z / total
	center = (p + z * z / (2 * total)) / denominator
	margin = z * math.sqrt((p * (1 - p) + z * z / (4 * total)) / total) / denominator
	return center - margin, center + margin


def binomial_tail_probability(successes: int, total: int) -> float | None:
	if total == 0:
		return None
	return sum(math.comb(total, k) for k in range(successes, total + 1)) / (2 ** total)


def two_sided_binomial_probability(successes: int, total: int) -> float | None:
	if total == 0:
		return None
	tail = min(successes, total - successes)
	probability = 2 * sum(math.comb(total, k) for k in range(0, tail + 1)) / (2 ** total)
	return min(1.0, probability)


def analyze_results(args: argparse.Namespace, results: list[GameResult]) -> dict:
	results_by_sample: dict[int, list[GameResult]] = {}
	for result in results:
		results_by_sample.setdefault(result.task.sample, []).append(result)

	valid_games = [result for result in results if is_valid_result(args, result)]
	if args.comparison_mode == "red-role":
		candidate_game_wins = sum(1 for result in valid_games if result.task.red_model == "candidate" and active_model_won(result))
		legacy_game_wins = sum(1 for result in valid_games if result.task.red_model == "legacy" and active_model_won(result))
	else:
		candidate_game_wins = sum(1 for result in valid_games if result.winner_model == "candidate")
		legacy_game_wins = sum(1 for result in valid_games if result.winner_model == "legacy")
	win_low, win_high = wilson_interval(candidate_game_wins, len(valid_games))

	candidate_sweeps = 0
	legacy_sweeps = 0
	splits = 0
	both_win = 0
	both_lose = 0
	invalid_samples = 0
	sample_rows: list[dict] = []

	for sample in sorted(results_by_sample):
		sample_results = sorted(results_by_sample[sample], key=lambda result: result.task.run)
		if len(sample_results) != 2 or any(not is_valid_result(args, result) for result in sample_results):
			invalid_samples += 1
			outcome = "invalid"
		elif args.comparison_mode == "red-role":
			legacy_result = next(result for result in sample_results if result.task.red_model == "legacy")
			candidate_result = next(result for result in sample_results if result.task.red_model == "candidate")
			legacy_win = active_model_won(legacy_result)
			candidate_win = active_model_won(candidate_result)
			if candidate_win and legacy_win:
				both_win += 1
				outcome = "both-win"
			elif not candidate_win and not legacy_win:
				both_lose += 1
				outcome = "both-lose"
			elif candidate_win:
				candidate_sweeps += 1
				outcome = "candidate-only"
			else:
				legacy_sweeps += 1
				outcome = "legacy-only"
		else:
			candidate_wins = sum(1 for result in sample_results if result.winner_model == "candidate")
			if candidate_wins == 2:
				candidate_sweeps += 1
				outcome = "candidate-sweep"
			elif candidate_wins == 0:
				legacy_sweeps += 1
				outcome = "legacy-sweep"
			else:
				splits += 1
				outcome = "split"

		sample_rows.append(
			{
				"sample": sample,
				"seed": sample_results[0].task.seed if sample_results else None,
				"map": sample_results[0].task.map_path if sample_results else None,
				"outcome": outcome,
				"winners": [result.winner_model for result in sample_results],
				"redRoleWins": [active_model_won(result) for result in sample_results] if args.comparison_mode == "red-role" else None,
			}
		)

	decisive = candidate_sweeps + legacy_sweeps
	one_sided_p = binomial_tail_probability(candidate_sweeps, decisive)
	two_sided_p = two_sided_binomial_probability(candidate_sweeps, decisive)

	return {
		"configuration": {
			"legacyAI": args.legacy_ai,
			"candidateAI": args.candidate_ai,
			"opponentAI": args.opponent_ai,
			"comparisonMode": args.comparison_mode,
			"samplesRequested": args.samples,
			"maps": args.maps or [],
			"randomMap": args.random_map,
			"randomMapOptions": {
				"size": args.randommap_size,
				"levels": args.randommap_levels,
				"players": args.randommap_players,
				"teams": args.randommap_teams,
				"compOnlyPlayers": args.randommap_comp_only_players,
				"compOnlyTeams": args.randommap_comp_only_teams,
				"water": args.randommap_water,
				"monsterStrength": args.randommap_monsters,
				"template": args.randommap_template,
			},
			"testdays": args.testdays,
			"adjudicateTestdays": args.adjudicate_testdays,
			"seedStart": args.seed_start,
			"seedStep": args.seed_step,
			"jobs": args.jobs,
		},
		"games": {
			"total": len(results),
			"valid": len(valid_games),
			"candidateWins": candidate_game_wins,
			"legacyWins": legacy_game_wins,
			"candidateWinRate": candidate_game_wins / len(valid_games) if valid_games else None,
			"candidateWinRateWilson95": [win_low, win_high],
		},
		"pairedSamples": {
			"total": args.samples,
			"candidateSweeps": candidate_sweeps,
			"legacySweeps": legacy_sweeps,
			"splits": splits,
			"bothWin": both_win,
			"bothLose": both_lose,
			"invalid": invalid_samples,
			"decisive": decisive,
			"oneSidedCandidateBetterP": one_sided_p,
			"twoSidedP": two_sided_p,
		},
		"sampleRows": sample_rows,
	}


def write_csv(output_dir: Path, results: list[GameResult]) -> None:
	fieldnames = [
		"sample",
		"run",
		"seed",
		"map",
		"direction",
		"redAI",
		"blueAI",
		"redModel",
		"blueModel",
		"winnerColor",
		"winnerModel",
		"testDayLimited",
		"adjudicated",
		"adjudicationDay",
		"adjudicationReason",
		"timedOut",
		"mapLoaded",
		"exitCode",
		"signal",
		"durationSeconds",
		"runDir",
	]
	with (output_dir / "games.csv").open("w", newline="") as handle:
		writer = csv.DictWriter(handle, fieldnames=fieldnames)
		writer.writeheader()
		for result in results:
			row = result_to_dict(result)
			writer.writerow({key: row[key] for key in fieldnames})


def print_summary(analysis: dict, output_dir: Path) -> None:
	games = analysis["games"]
	paired = analysis["pairedSamples"]
	print()
	print("Predictor A/B summary:")
	print(f"valid games: {games['valid']}/{games['total']}")
	print(
		"game wins: "
		f"candidate={games['candidateWins']} legacy={games['legacyWins']} "
		f"candidateRate={games['candidateWinRate']}"
	)
	if analysis["configuration"]["comparisonMode"] == "red-role":
		print(
			"paired samples: "
			f"candidateOnly={paired['candidateSweeps']} "
			f"legacyOnly={paired['legacySweeps']} "
			f"bothWin={paired['bothWin']} bothLose={paired['bothLose']} "
			f"invalid={paired['invalid']} decisive={paired['decisive']}"
		)
	else:
		print(
			"paired samples: "
			f"candidateSweeps={paired['candidateSweeps']} "
			f"legacySweeps={paired['legacySweeps']} "
			f"splits={paired['splits']} invalid={paired['invalid']} "
			f"decisive={paired['decisive']}"
		)
	print(
		"sign test: "
		f"oneSidedCandidateBetterP={paired['oneSidedCandidateBetterP']} "
		f"twoSidedP={paired['twoSidedP']}"
	)
	print(f"raw logs and summaries: {output_dir}")


def main() -> int:
	args = parse_args()
	output_dir = args.output_dir or default_output_dir()
	output_dir.mkdir(parents=True, exist_ok=True)

	tasks = build_tasks(args, output_dir)
	(output_dir / "tasks.json").write_text(
		json.dumps([task.__dict__ | {"run_dir": str(task.run_dir)} for task in tasks], indent=2, default=str) + "\n"
	)

	results: list[GameResult] = []
	with ThreadPoolExecutor(max_workers=args.jobs) as executor:
		future_to_task = {executor.submit(run_game, args, task): task for task in tasks}
		for future in as_completed(future_to_task):
			task = future_to_task[future]
			try:
				result = future.result()
			except Exception as exc:
				print(f"sample={task.sample:04d} direction={task.direction} failed: {exc}", file=sys.stderr)
				continue

			results.append(result)
			winner = result.winner_model or "-"
			adjudicated = " adjudicated" if result.adjudicated else ""
			status = "timeout" if result.timed_out else process_status(result.exit_code)["exitCode"]
			print(
				f"sample={task.sample:04d} direction={task.direction} "
				f"winner={winner}{adjudicated} loaded={str(result.map_loaded).lower()} "
				f"exit={status} duration={result.duration_seconds:.1f}s"
			)

	results.sort(key=lambda result: (result.task.sample, result.task.run))
	(output_dir / "games.json").write_text(
		json.dumps([result_to_dict(result) for result in results], indent=2) + "\n"
	)
	write_csv(output_dir, results)
	analysis = analyze_results(args, results)
	(output_dir / "summary.json").write_text(json.dumps(analysis, indent=2) + "\n")
	print_summary(analysis, output_dir)

	expected_games = args.samples * 2
	if len(results) != expected_games:
		return 2
	if any(not is_valid_result(args, result) for result in results):
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
