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
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
MAP_LOADED_RE = re.compile(r"\bMap loaded!")
WINNER_RE = re.compile(r"\b([A-Za-z]+) player won\. Ending game\.")
LOSER_RE = re.compile(r"\b([A-Za-z]+) player lost\. Ending game\.")
TEST_DAY_LIMIT_RE = re.compile(r"\bReached test day limit\b")
RUNTIME_SIMULATION_STATS_RE = re.compile(
	r"Runtime battle simulation stats for player \d+ \(([^)]+)\): "
	r"requests (\d+), complete (\d+), incomplete (\d+), safe (\d+), rejected (\d+)"
	r"(?:, invalid (\d+), not available (\d+))?"
	r"(?:, skipped no target (\d+))?"
	r"(?:, cache hits (\d+))?"
	r"(?:, planning accepted (\d+), planning rejected (\d+), planning incomplete (\d+))?"
	r"(?:, planning accepted static safe (\d+), planning accepted static unsafe (\d+), planning rejected static safe (\d+), planning rejected static unsafe (\d+))?"
	r"(?:, planning cache hits (\d+))?"
	r"(?:, planning skipped future turn (\d+), planning skipped unsafe path (\d+), planning skipped projected army (\d+), planning skipped no target (\d+))?"
)
PLANNER_SIMULATION_RE = re.compile(
	r"Planner battle simulation (accepted|rejected|incomplete)(?: .*?)? for player \d+ \(([^)]+)\):"
)

RUNTIME_SIMULATION_FIELDS = [
	"requests",
	"complete",
	"incomplete",
	"safe",
	"rejected",
	"invalid",
	"notAvailable",
	"skippedNoTarget",
	"cacheHits",
	"planningAccepted",
	"planningRejected",
	"planningIncomplete",
	"planningAcceptedStaticSafe",
	"planningAcceptedStaticUnsafe",
	"planningRejectedStaticSafe",
	"planningRejectedStaticUnsafe",
	"planningCacheHits",
	"planningSkippedFutureTurn",
	"planningSkippedUnsafePath",
	"planningSkippedProjectedArmy",
	"planningSkippedNoTarget",
]

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

ACTIVE_PROCESSES: set[subprocess.Popen[bytes]] = set()
ACTIVE_PROCESSES_LOCK = threading.Lock()


@dataclass(frozen=True)
class ConfigReplacement:
	file: Path
	old: str
	new: str


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
	runtime_simulation_stats_by_color: dict[str, dict[str, int]]
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
	parser.add_argument(
		"--config-replace",
		nargs=3,
		action="append",
		default=[],
		metavar=("FILE", "OLD", "NEW"),
		help=(
			"Temporarily replace literal OLD with NEW in FILE for the whole comparison run. "
			"FILE is resolved relative to --workdir when not absolute. May be repeated."
		),
	)
	parser.add_argument(
		"--require-runtime-simulation",
		action="append",
		default=[],
		choices=("legacy", "candidate", "opponent"),
		metavar="MODEL",
		help=(
			"Require runtime battle simulation evidence for MODEL in valid games. "
			"May be repeated; intended for V3 runtime-fallback A/B runs."
		),
	)
	parser.add_argument(
		"--min-runtime-simulation-requests",
		type=int,
		default=1,
		help="Minimum runtime simulation requests required for each --require-runtime-simulation model.",
	)
	parser.add_argument(
		"--min-runtime-simulation-complete-rate",
		type=float,
		default=0.9,
		help="Minimum complete/request ratio required for each --require-runtime-simulation model.",
	)
	parser.add_argument(
		"--max-runtime-simulation-incomplete",
		type=int,
		default=None,
		help="Maximum incomplete runtime simulation responses allowed for each --require-runtime-simulation model.",
	)
	parser.add_argument(
		"--max-runtime-simulation-invalid",
		type=int,
		default=None,
		help="Maximum invalid runtime simulation requests allowed for each --require-runtime-simulation model.",
	)
	parser.add_argument(
		"--max-runtime-simulation-not-available",
		type=int,
		default=None,
		help="Maximum not-available runtime simulation responses allowed for each --require-runtime-simulation model.",
	)
	parser.add_argument(
		"--min-runtime-simulation-planning-decisions",
		type=int,
		default=0,
		help=(
			"Minimum completed planner-side simulation decisions required for each "
			"--require-runtime-simulation model. Counts planningAccepted + planningRejected."
		),
	)
	parser.add_argument(
		"--min-runtime-simulation-planning-vetoes",
		type=int,
		default=0,
		help=(
			"Minimum planner-side vetoes required for each --require-runtime-simulation model. "
			"Counts static-safe targets rejected by simulation."
		),
	)
	parser.add_argument(
		"--min-runtime-simulation-planning-rescues",
		type=int,
		default=0,
		help=(
			"Minimum planner-side rescues required for each --require-runtime-simulation model. "
			"Counts static-unsafe targets accepted by simulation."
		),
	)
	parser.add_argument(
		"--max-runtime-simulation-planning-incomplete",
		type=int,
		default=None,
		help="Maximum incomplete planner-side simulation responses allowed for each --require-runtime-simulation model.",
	)
	parser.add_argument(
		"--max-candidate-better-p",
		type=float,
		default=None,
		help="Maximum one-sided paired sign-test p-value allowed for the candidate-better hypothesis.",
	)
	parser.add_argument(
		"--min-candidate-win-rate",
		type=float,
		default=None,
		help="Minimum valid-game candidate win rate required.",
	)
	parser.add_argument(
		"--min-candidate-win-rate-wilson-lower",
		type=float,
		default=None,
		help="Minimum Wilson 95% lower bound for the valid-game candidate win rate.",
	)
	parser.add_argument(
		"--min-valid-games",
		type=int,
		default=None,
		help="Minimum valid games required for outcome proof.",
	)
	parser.add_argument(
		"--max-invalid-paired-samples",
		type=int,
		default=None,
		help="Maximum invalid paired samples allowed for outcome proof.",
	)
	parser.add_argument(
		"--min-paired-decisive-samples",
		type=int,
		default=None,
		help="Minimum decisive paired samples required for outcome proof.",
	)
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
	if args.min_runtime_simulation_requests <= 0:
		parser.error("--min-runtime-simulation-requests must be positive")
	if not 0.0 <= args.min_runtime_simulation_complete_rate <= 1.0:
		parser.error("--min-runtime-simulation-complete-rate must be between 0 and 1")
	for option_name in (
		"max_runtime_simulation_incomplete",
		"max_runtime_simulation_invalid",
		"max_runtime_simulation_not_available",
		"max_runtime_simulation_planning_incomplete",
		"min_valid_games",
		"max_invalid_paired_samples",
		"min_paired_decisive_samples",
	):
		if getattr(args, option_name) is not None and getattr(args, option_name) < 0:
			parser.error("--" + option_name.replace("_", "-") + " must be non-negative")
	for option_name in (
		"max_candidate_better_p",
		"min_candidate_win_rate",
		"min_candidate_win_rate_wilson_lower",
	):
		value = getattr(args, option_name)
		if value is not None and not 0.0 <= value <= 1.0:
			parser.error("--" + option_name.replace("_", "-") + " must be between 0 and 1")

	return args


def default_output_dir() -> Path:
	timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
	return Path("predictor-ab-runs") / timestamp


def canonical_color(value: str | None) -> str | None:
	if not value:
		return None
	return COLOR_ALIASES.get(value.lower(), value)


def empty_runtime_simulation_stats() -> dict[str, int]:
	return {key: 0 for key in RUNTIME_SIMULATION_FIELDS}


def add_runtime_simulation_stats(target: dict[str, int], source: dict[str, int]) -> None:
	for key in RUNTIME_SIMULATION_FIELDS:
		target[key] = target.get(key, 0) + source.get(key, 0)


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


def register_active_process(proc: subprocess.Popen[bytes]) -> None:
	with ACTIVE_PROCESSES_LOCK:
		ACTIVE_PROCESSES.add(proc)


def unregister_active_process(proc: subprocess.Popen[bytes]) -> None:
	with ACTIVE_PROCESSES_LOCK:
		ACTIVE_PROCESSES.discard(proc)


def active_processes_snapshot() -> list[subprocess.Popen[bytes]]:
	with ACTIVE_PROCESSES_LOCK:
		return list(ACTIVE_PROCESSES)


def install_config_restore_signal_handlers(originals: dict[Path, bytes]) -> dict[int, object]:
	previous_handlers = {}

	def restore_and_exit(signum, _frame) -> None:
		for proc in active_processes_snapshot():
			if proc.poll() is None:
				terminate_process(proc)
		restore_config_files(originals)
		signal_name = signal.Signals(signum).name
		print(f"Restored config replacements after {signal_name}.", file=sys.stderr, flush=True)
		os._exit(128 + signum)

	for signum in (signal.SIGINT, signal.SIGTERM):
		previous_handlers[signum] = signal.getsignal(signum)
		signal.signal(signum, restore_and_exit)

	return previous_handlers


def restore_signal_handlers(previous_handlers: dict[int, object]) -> None:
	for signum, handler in previous_handlers.items():
		signal.signal(signum, handler)


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


def parse_run_logs(task: GameTask) -> tuple[bool, str | None, str | None, bool, dict[str, dict[str, int]], list[str]]:
	map_loaded = False
	winner = None
	loser = None
	test_day_limited = False
	runtime_simulation_stats_by_color: dict[str, dict[str, int]] = {}
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

		stats_match = RUNTIME_SIMULATION_STATS_RE.search(line)
		if stats_match:
			color = canonical_color(stats_match.group(1))
			if color:
				stats = runtime_simulation_stats_by_color.setdefault(color, empty_runtime_simulation_stats())
				add_runtime_simulation_stats(
					stats,
					{
						"requests": int(stats_match.group(2)),
						"complete": int(stats_match.group(3)),
						"incomplete": int(stats_match.group(4)),
						"safe": int(stats_match.group(5)),
						"rejected": int(stats_match.group(6)),
						"invalid": int(stats_match.group(7) or 0),
						"notAvailable": int(stats_match.group(8) or 0),
						"skippedNoTarget": int(stats_match.group(9) or 0),
						"cacheHits": int(stats_match.group(10) or 0),
						"planningAccepted": int(stats_match.group(11) or 0),
						"planningRejected": int(stats_match.group(12) or 0),
							"planningIncomplete": int(stats_match.group(13) or 0),
							"planningAcceptedStaticSafe": int(stats_match.group(14) or 0),
							"planningAcceptedStaticUnsafe": int(stats_match.group(15) or 0),
							"planningRejectedStaticSafe": int(stats_match.group(16) or 0),
							"planningRejectedStaticUnsafe": int(stats_match.group(17) or 0),
							"planningCacheHits": int(stats_match.group(18) or 0),
							"planningSkippedFutureTurn": int(stats_match.group(19) or 0),
							"planningSkippedUnsafePath": int(stats_match.group(20) or 0),
							"planningSkippedProjectedArmy": int(stats_match.group(21) or 0),
							"planningSkippedNoTarget": int(stats_match.group(22) or 0),
						},
					)

		planner_match = PLANNER_SIMULATION_RE.search(line)
		if planner_match:
			color = canonical_color(planner_match.group(2))
			if color:
				stats = runtime_simulation_stats_by_color.setdefault(color, empty_runtime_simulation_stats())
				field = {
					"accepted": "planningAccepted",
					"rejected": "planningRejected",
					"incomplete": "planningIncomplete",
				}[planner_match.group(1)]
				stats[field] += 1

	if winner is None and loser is not None:
		winner = opposite_two_player_color(loser)

	return map_loaded, winner, loser, test_day_limited, runtime_simulation_stats_by_color, diagnostics[-20:]


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
		register_active_process(proc)

		try:
			try:
				exit_code = proc.wait(timeout=args.timeout)
			except subprocess.TimeoutExpired:
				timed_out = True
				terminate_process(proc)
				exit_code = proc.returncode
				stdout.write(f"\nTimed out after {args.timeout} seconds.\n".encode())
		finally:
			unregister_active_process(proc)

	duration = time.monotonic() - start
	map_loaded, winner_color, loser_color, test_day_limited, runtime_simulation_stats_by_color, diagnostics = parse_run_logs(task)
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
		runtime_simulation_stats_by_color=runtime_simulation_stats_by_color,
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
	runtime_stats_by_model: dict[str, dict[str, int]] = {}
	runtime_stats_total = empty_runtime_simulation_stats()
	for color, stats in result.runtime_simulation_stats_by_color.items():
		add_runtime_simulation_stats(runtime_stats_total, stats)
		model = model_for_color(result.task, canonical_color(color))
		if model:
			add_runtime_simulation_stats(
				runtime_stats_by_model.setdefault(model, empty_runtime_simulation_stats()),
				stats,
			)
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
		"runtimeBattleSimulationStats": runtime_stats_total,
		"runtimeBattleSimulationStatsByColor": result.runtime_simulation_stats_by_color,
		"runtimeBattleSimulationStatsByModel": runtime_stats_by_model,
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
	runtime_stats_by_model: dict[str, dict[str, int]] = {}
	runtime_stats_total = empty_runtime_simulation_stats()
	for result in valid_games:
		for color, stats in result.runtime_simulation_stats_by_color.items():
			add_runtime_simulation_stats(runtime_stats_total, stats)
			model = model_for_color(result.task, canonical_color(color))
			if model:
				add_runtime_simulation_stats(
					runtime_stats_by_model.setdefault(model, empty_runtime_simulation_stats()),
					stats,
				)
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
			"runtimeSimulationRequirements": {
				"models": args.require_runtime_simulation,
				"minRequests": args.min_runtime_simulation_requests,
				"minCompleteRate": args.min_runtime_simulation_complete_rate,
				"maxIncomplete": args.max_runtime_simulation_incomplete,
				"maxInvalid": args.max_runtime_simulation_invalid,
				"maxNotAvailable": args.max_runtime_simulation_not_available,
				"minPlanningDecisions": args.min_runtime_simulation_planning_decisions,
				"minPlanningVetoes": args.min_runtime_simulation_planning_vetoes,
				"minPlanningRescues": args.min_runtime_simulation_planning_rescues,
				"maxPlanningIncomplete": args.max_runtime_simulation_planning_incomplete,
			},
			"outcomeRequirements": {
				"maxCandidateBetterP": args.max_candidate_better_p,
				"minCandidateWinRate": args.min_candidate_win_rate,
				"minCandidateWinRateWilsonLower": args.min_candidate_win_rate_wilson_lower,
				"minValidGames": args.min_valid_games,
				"maxInvalidPairedSamples": args.max_invalid_paired_samples,
				"minPairedDecisiveSamples": args.min_paired_decisive_samples,
			},
			"configReplacements": [
				{"file": str(replacement.file), "old": replacement.old, "new": replacement.new}
				for replacement in resolve_config_replacements(args)
			],
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
		"runtimeBattleSimulation": {
			"total": runtime_stats_total,
			"byModel": runtime_stats_by_model,
		},
		"sampleRows": sample_rows,
	}


def evaluate_runtime_simulation_requirements(args: argparse.Namespace, analysis: dict) -> dict:
	errors: list[str] = []
	model_reports: list[dict] = []
	stats_by_model = analysis["runtimeBattleSimulation"]["byModel"]

	for model in args.require_runtime_simulation:
		stats = stats_by_model.get(model, empty_runtime_simulation_stats())
		requests = stats["requests"]
		complete = stats["complete"]
		complete_rate = complete / requests if requests else None
		planning_decisions = stats["planningAccepted"] + stats["planningRejected"]
		planning_vetoes = stats["planningRejectedStaticSafe"]
		planning_rescues = stats["planningAcceptedStaticUnsafe"]
		model_errors = []

		if requests < args.min_runtime_simulation_requests:
			model_errors.append(
				f"{model}: runtime requests {requests} below required "
				f"{args.min_runtime_simulation_requests}"
			)
		if complete_rate is not None and complete_rate < args.min_runtime_simulation_complete_rate:
			model_errors.append(
				f"{model}: runtime complete rate {complete_rate:.3f} below required "
				f"{args.min_runtime_simulation_complete_rate:.3f}"
			)
		if args.max_runtime_simulation_incomplete is not None and stats["incomplete"] > args.max_runtime_simulation_incomplete:
			model_errors.append(
				f"{model}: runtime incomplete responses {stats['incomplete']} above allowed "
				f"{args.max_runtime_simulation_incomplete}"
			)
		if args.max_runtime_simulation_invalid is not None and stats["invalid"] > args.max_runtime_simulation_invalid:
			model_errors.append(
				f"{model}: runtime invalid requests {stats['invalid']} above allowed "
				f"{args.max_runtime_simulation_invalid}"
			)
		if args.max_runtime_simulation_not_available is not None and stats["notAvailable"] > args.max_runtime_simulation_not_available:
			model_errors.append(
				f"{model}: runtime not-available responses {stats['notAvailable']} above allowed "
				f"{args.max_runtime_simulation_not_available}"
			)
		if planning_decisions < args.min_runtime_simulation_planning_decisions:
			model_errors.append(
				f"{model}: planner simulation decisions {planning_decisions} below required "
				f"{args.min_runtime_simulation_planning_decisions}"
			)
		if planning_vetoes < args.min_runtime_simulation_planning_vetoes:
			model_errors.append(
				f"{model}: planner simulation vetoes {planning_vetoes} below required "
				f"{args.min_runtime_simulation_planning_vetoes}"
			)
		if planning_rescues < args.min_runtime_simulation_planning_rescues:
			model_errors.append(
				f"{model}: planner simulation rescues {planning_rescues} below required "
				f"{args.min_runtime_simulation_planning_rescues}"
			)
		if args.max_runtime_simulation_planning_incomplete is not None and stats["planningIncomplete"] > args.max_runtime_simulation_planning_incomplete:
			model_errors.append(
				f"{model}: planner simulation incomplete responses {stats['planningIncomplete']} above allowed "
				f"{args.max_runtime_simulation_planning_incomplete}"
			)

		errors.extend(model_errors)
		model_reports.append(
			{
				"model": model,
				"stats": stats,
				"completeRate": complete_rate,
				"planningDecisions": planning_decisions,
				"planningVetoes": planning_vetoes,
				"planningRescues": planning_rescues,
				"ok": not model_errors,
				"errors": model_errors,
			}
		)

	return {
		"required": bool(args.require_runtime_simulation),
		"models": model_reports,
		"minRequests": args.min_runtime_simulation_requests,
		"minCompleteRate": args.min_runtime_simulation_complete_rate,
		"maxIncomplete": args.max_runtime_simulation_incomplete,
		"maxInvalid": args.max_runtime_simulation_invalid,
		"maxNotAvailable": args.max_runtime_simulation_not_available,
		"minPlanningDecisions": args.min_runtime_simulation_planning_decisions,
		"minPlanningVetoes": args.min_runtime_simulation_planning_vetoes,
		"minPlanningRescues": args.min_runtime_simulation_planning_rescues,
		"maxPlanningIncomplete": args.max_runtime_simulation_planning_incomplete,
		"ok": not errors,
		"errors": errors,
	}


def evaluate_outcome_requirements(args: argparse.Namespace, analysis: dict) -> dict:
	errors: list[str] = []
	games = analysis["games"]
	paired = analysis["pairedSamples"]

	if args.min_valid_games is not None:
		valid_games = games["valid"]
		if valid_games < args.min_valid_games:
			errors.append(
				f"valid games {valid_games} below required "
				f"{args.min_valid_games}"
			)

	if args.max_invalid_paired_samples is not None:
		invalid_samples = paired["invalid"]
		if invalid_samples > args.max_invalid_paired_samples:
			errors.append(
				f"invalid paired samples {invalid_samples} above allowed "
				f"{args.max_invalid_paired_samples}"
			)

	if args.min_paired_decisive_samples is not None:
		decisive_samples = paired["decisive"]
		if decisive_samples < args.min_paired_decisive_samples:
			errors.append(
				f"decisive paired samples {decisive_samples} below required "
				f"{args.min_paired_decisive_samples}"
			)

	if args.max_candidate_better_p is not None:
		p_value = paired["oneSidedCandidateBetterP"]
		if p_value is None:
			errors.append("candidate-better p-value is unavailable")
		elif p_value > args.max_candidate_better_p:
			errors.append(
				f"candidate-better p-value {p_value:.6g} above allowed "
				f"{args.max_candidate_better_p:.6g}"
			)

	if args.min_candidate_win_rate is not None:
		win_rate = games["candidateWinRate"]
		if win_rate is None:
			errors.append("candidate win rate is unavailable")
		elif win_rate < args.min_candidate_win_rate:
			errors.append(
				f"candidate win rate {win_rate:.6g} below required "
				f"{args.min_candidate_win_rate:.6g}"
			)

	if args.min_candidate_win_rate_wilson_lower is not None:
		wilson_lower = games["candidateWinRateWilson95"][0]
		if wilson_lower is None:
			errors.append("candidate win-rate Wilson lower bound is unavailable")
		elif wilson_lower < args.min_candidate_win_rate_wilson_lower:
			errors.append(
				f"candidate win-rate Wilson lower bound {wilson_lower:.6g} below required "
				f"{args.min_candidate_win_rate_wilson_lower:.6g}"
			)

	required = any(
		value is not None
		for value in (
			args.max_candidate_better_p,
			args.min_candidate_win_rate,
			args.min_candidate_win_rate_wilson_lower,
			args.min_valid_games,
			args.max_invalid_paired_samples,
			args.min_paired_decisive_samples,
		)
	)
	return {
		"required": required,
		"maxCandidateBetterP": args.max_candidate_better_p,
		"minCandidateWinRate": args.min_candidate_win_rate,
		"minCandidateWinRateWilsonLower": args.min_candidate_win_rate_wilson_lower,
		"minValidGames": args.min_valid_games,
		"maxInvalidPairedSamples": args.max_invalid_paired_samples,
		"minPairedDecisiveSamples": args.min_paired_decisive_samples,
		"ok": not errors,
		"errors": errors,
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
		"runtimeRequests",
		"runtimeComplete",
		"runtimeIncomplete",
		"runtimeSafe",
		"runtimeRejected",
		"runtimeCacheHits",
		"planningAccepted",
		"planningRejected",
		"planningIncomplete",
		"planningAcceptedStaticSafe",
		"planningAcceptedStaticUnsafe",
		"planningRejectedStaticSafe",
		"planningRejectedStaticUnsafe",
		"planningCacheHits",
		"planningSkippedFutureTurn",
		"planningSkippedUnsafePath",
		"planningSkippedProjectedArmy",
		"planningSkippedNoTarget",
		"runDir",
	]
	with (output_dir / "games.csv").open("w", newline="") as handle:
		writer = csv.DictWriter(handle, fieldnames=fieldnames)
		writer.writeheader()
		for result in results:
			row = result_to_dict(result)
			runtime_stats = row["runtimeBattleSimulationStats"]
			row.update(
				{
					"runtimeRequests": runtime_stats["requests"],
					"runtimeComplete": runtime_stats["complete"],
					"runtimeIncomplete": runtime_stats["incomplete"],
					"runtimeSafe": runtime_stats["safe"],
					"runtimeRejected": runtime_stats["rejected"],
					"runtimeCacheHits": runtime_stats["cacheHits"],
					"planningAccepted": runtime_stats["planningAccepted"],
					"planningRejected": runtime_stats["planningRejected"],
					"planningIncomplete": runtime_stats["planningIncomplete"],
					"planningAcceptedStaticSafe": runtime_stats["planningAcceptedStaticSafe"],
					"planningAcceptedStaticUnsafe": runtime_stats["planningAcceptedStaticUnsafe"],
					"planningRejectedStaticSafe": runtime_stats["planningRejectedStaticSafe"],
					"planningRejectedStaticUnsafe": runtime_stats["planningRejectedStaticUnsafe"],
					"planningCacheHits": runtime_stats["planningCacheHits"],
					"planningSkippedFutureTurn": runtime_stats["planningSkippedFutureTurn"],
					"planningSkippedUnsafePath": runtime_stats["planningSkippedUnsafePath"],
					"planningSkippedProjectedArmy": runtime_stats["planningSkippedProjectedArmy"],
					"planningSkippedNoTarget": runtime_stats["planningSkippedNoTarget"],
				}
			)
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
	runtime_stats = analysis["runtimeBattleSimulation"]
	print(f"runtime simulation total: {runtime_stats['total']}")
	for model, stats in sorted(runtime_stats["byModel"].items()):
		print(f"runtime simulation {model}: {stats}")
	requirements = runtime_stats.get("requirements")
	if requirements and requirements["required"]:
		status = "ok" if requirements["ok"] else "failed"
		print(f"runtime simulation requirements: {status}")
	outcome_requirements = analysis.get("outcomeRequirements")
	if outcome_requirements and outcome_requirements["required"]:
		status = "ok" if outcome_requirements["ok"] else "failed"
		print(f"outcome requirements: {status}")
	print(f"raw logs and summaries: {output_dir}")


def main() -> int:
	args = parse_args()
	output_dir = args.output_dir or default_output_dir()
	output_dir.mkdir(parents=True, exist_ok=True)
	replacements = resolve_config_replacements(args)
	originals: dict[Path, bytes] = {}
	results: list[GameResult] = []
	runtime_requirements: dict | None = None
	outcome_requirements: dict | None = None
	previous_signal_handlers = install_config_restore_signal_handlers(originals) if replacements else {}

	try:
		apply_config_replacements(replacements, originals)

		tasks = build_tasks(args, output_dir)
		(output_dir / "tasks.json").write_text(
			json.dumps([task.__dict__ | {"run_dir": str(task.run_dir)} for task in tasks], indent=2, default=str) + "\n"
		)

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
		runtime_requirements = evaluate_runtime_simulation_requirements(args, analysis)
		analysis["runtimeBattleSimulation"]["requirements"] = runtime_requirements
		outcome_requirements = evaluate_outcome_requirements(args, analysis)
		analysis["outcomeRequirements"] = outcome_requirements
		(output_dir / "summary.json").write_text(json.dumps(analysis, indent=2) + "\n")
		print_summary(analysis, output_dir)
		for error in runtime_requirements["errors"]:
			print(f"runtime simulation requirement failed: {error}", file=sys.stderr)
		for error in outcome_requirements["errors"]:
			print(f"outcome requirement failed: {error}", file=sys.stderr)
	finally:
		restore_config_files(originals)
		restore_signal_handlers(previous_signal_handlers)

	expected_games = args.samples * 2
	if len(results) != expected_games:
		return 2
	if any(not is_valid_result(args, result) for result in results):
		return 1
	if runtime_requirements and not runtime_requirements["ok"]:
		return 3
	if outcome_requirements and not outcome_requirements["ok"]:
		return 4
	return 0


if __name__ == "__main__":
	sys.exit(main())
