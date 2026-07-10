/*
 * EntryPoint.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace
{
struct Options
{
	std::filesystem::path clientPath = "vcmiclient";
	std::vector<std::string> maps;
	std::filesystem::path mapsFile;
	std::filesystem::path outputDirectory;
	std::string combatAI = "BattleAI";
	std::string generatedMode = "mixed";
	std::vector<std::string> extraClientArgs;
	uint64_t battles = 0;
	uint64_t shards = 0;
	unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
	int64_t globalSeed = 1;
	bool generateMap = false;
	bool dryRun = false;
};

struct Shard
{
	uint64_t index = 0;
	uint64_t battles = 0;
	int64_t seed = 0;
	std::string map;
	std::filesystem::path outputPath;
	std::filesystem::path logPath;
};

std::mutex outputMutex;

void printHelp()
{
	std::cout
		<< "Usage: vcmibattlesim --testmap MAP --output-dir DIR --battles N [options]\n\n"
		<< "Options:\n"
		<< "  --client PATH              vcmiclient executable path, default: vcmiclient\n"
		<< "  --testmap PATH             battle-only map to run, can be repeated\n"
		<< "  --maps-file PATH           newline-separated list of maps\n"
		<< "  --generate-map             generate deterministic battle-only maps per shard\n"
		<< "  --generated-mode MODE      generated map mode: mixed, hero, or monster\n"
		<< "  --output-dir DIR           directory for shard JSONL and logs\n"
		<< "  --battles N                total battle result rows to collect\n"
		<< "  --jobs N                   concurrent client processes\n"
		<< "  --shards N                 total client shards, default: max(jobs, maps)\n"
		<< "  --seed N                   global deterministic seed, default: 1\n"
		<< "  --combat-ai NAME           combat AI passed to the client, default: BattleAI\n"
		<< "  --extra-client-arg ARG     extra argument passed to every client, can be repeated\n"
		<< "  --dry-run                  print commands without running them\n"
		<< "  --help                     display this help and exit\n";
}

uint64_t parsePositive(const std::string & value, const std::string & option)
{
	size_t parsed = 0;
	uint64_t result = 0;
	try
	{
		result = std::stoull(value, &parsed, 10);
	}
	catch(const std::exception &)
	{
		throw std::runtime_error("Invalid numeric value for " + option + ": " + value);
	}
	if(parsed != value.size() || result == 0)
		throw std::runtime_error("Invalid numeric value for " + option + ": " + value);
	return result;
}

int64_t parseInteger(const std::string & value, const std::string & option)
{
	size_t parsed = 0;
	int64_t result = 0;
	try
	{
		result = std::stoll(value, &parsed, 10);
	}
	catch(const std::exception &)
	{
		throw std::runtime_error("Invalid numeric value for " + option + ": " + value);
	}
	if(parsed != value.size())
		throw std::runtime_error("Invalid numeric value for " + option + ": " + value);
	return result;
}

std::string requireValue(int argc, char ** argv, int & index, const std::string & option)
{
	if(index + 1 >= argc)
		throw std::runtime_error("Missing value for " + option);
	++index;
	return argv[index];
}

Options parseOptions(int argc, char ** argv)
{
	Options options;
	for(int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if(arg == "--help" || arg == "-h")
		{
			printHelp();
			std::exit(0);
		}
		else if(arg == "--client")
			options.clientPath = requireValue(argc, argv, i, arg);
		else if(arg == "--testmap" || arg == "--map")
			options.maps.push_back(requireValue(argc, argv, i, arg));
		else if(arg == "--maps-file")
			options.mapsFile = requireValue(argc, argv, i, arg);
		else if(arg == "--generate-map")
			options.generateMap = true;
		else if(arg == "--generated-mode")
			options.generatedMode = requireValue(argc, argv, i, arg);
		else if(arg == "--output-dir")
			options.outputDirectory = requireValue(argc, argv, i, arg);
		else if(arg == "--battles")
			options.battles = parsePositive(requireValue(argc, argv, i, arg), arg);
		else if(arg == "--jobs")
			options.jobs = static_cast<unsigned>(parsePositive(requireValue(argc, argv, i, arg), arg));
		else if(arg == "--shards")
			options.shards = parsePositive(requireValue(argc, argv, i, arg), arg);
		else if(arg == "--seed")
			options.globalSeed = parseInteger(requireValue(argc, argv, i, arg), arg);
		else if(arg == "--combat-ai")
			options.combatAI = requireValue(argc, argv, i, arg);
		else if(arg == "--extra-client-arg")
			options.extraClientArgs.push_back(requireValue(argc, argv, i, arg));
		else if(arg == "--dry-run")
			options.dryRun = true;
		else
			throw std::runtime_error("Unknown option: " + arg);
	}
	return options;
}

std::vector<std::string> readMaps(const Options & options)
{
	std::vector<std::string> maps = options.maps;
	if(!options.mapsFile.empty())
	{
		std::ifstream input(options.mapsFile);
		if(!input)
			throw std::runtime_error("Unable to open maps file: " + options.mapsFile.string());

		std::string line;
		while(std::getline(input, line))
		{
			line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) { return !std::isspace(ch); }));
			line.erase(std::find_if(line.rbegin(), line.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), line.end());
			if(line.empty() || line.front() == '#')
				continue;
			maps.push_back(line);
		}
	}
	if(maps.empty() && !options.generateMap)
		throw std::runtime_error("At least one --testmap or --maps-file entry is required");
	return maps;
}

void validateOptions(Options & options, const std::vector<std::string> & maps)
{
	if(options.outputDirectory.empty())
		throw std::runtime_error("--output-dir is required");
	if(options.battles == 0)
		throw std::runtime_error("--battles is required");
	if(options.jobs == 0)
		throw std::runtime_error("--jobs must be positive");
	if(options.generatedMode != "mixed" && options.generatedMode != "hero" && options.generatedMode != "monster")
		throw std::runtime_error("--generated-mode must be mixed, hero, or monster");
	if(options.shards == 0)
		options.shards = std::max<uint64_t>(options.jobs, options.generateMap ? 1 : maps.size());
	if(options.shards > options.battles)
		options.shards = options.battles;
	options.jobs = static_cast<unsigned>(std::min<uint64_t>(options.jobs, options.shards));
}

uint64_t splitmix64(uint64_t value)
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

int64_t shardSeed(int64_t globalSeed, uint64_t shardIndex)
{
	const auto mixed = splitmix64(static_cast<uint64_t>(globalSeed) + shardIndex);
	return static_cast<int64_t>(mixed & 0x7fffffffffffffffULL);
}

std::string shardName(uint64_t index, const std::string & extension)
{
	std::ostringstream out;
	out << "shard-";
	out.width(5);
	out.fill('0');
	out << index << extension;
	return out.str();
}

std::vector<Shard> makeShards(const Options & options, const std::vector<std::string> & maps)
{
	std::vector<Shard> shards;
	shards.reserve(options.shards);

	const uint64_t base = options.battles / options.shards;
	const uint64_t remainder = options.battles % options.shards;

	for(uint64_t i = 0; i < options.shards; ++i)
	{
		Shard shard;
		shard.index = i;
		shard.battles = base + (i < remainder ? 1 : 0);
		shard.seed = shardSeed(options.globalSeed, i);
		if(!maps.empty())
			shard.map = maps[i % maps.size()];
		shard.outputPath = options.outputDirectory / shardName(i, ".jsonl");
		shard.logPath = options.outputDirectory / shardName(i, ".log");
		shards.push_back(std::move(shard));
	}

	return shards;
}

std::string shellQuote(const std::string & value)
{
	std::string result = "'";
	for(char ch : value)
	{
		if(ch == '\'')
			result += "'\\''";
		else
			result += ch;
	}
	result += "'";
	return result;
}

std::string jsonQuote(const std::string & value)
{
	std::string result = "\"";
	for(char ch : value)
	{
		switch(ch)
		{
			case '\\':
				result += "\\\\";
				break;
			case '"':
				result += "\\\"";
				break;
			case '\n':
				result += "\\n";
				break;
			case '\r':
				result += "\\r";
				break;
			case '\t':
				result += "\\t";
				break;
			default:
				result += ch;
				break;
		}
	}
	result += '"';
	return result;
}

std::string makeCommand(const Options & options, const Shard & shard, uint64_t shardCount)
{
	std::ostringstream command;
	command << shellQuote(options.clientPath.string())
		<< " --battle-sim-output " << shellQuote(shard.outputPath.string())
		<< " --battle-sim-max-battles " << shard.battles
		<< " --battle-sim-shard-index " << shard.index
		<< " --battle-sim-shard-count " << shardCount
		<< " --battle-sim-seed " << shard.seed
		<< " --battle-sim-global-seed " << options.globalSeed
		<< " --battle-sim-combat-ai " << shellQuote(options.combatAI);

	if(options.generateMap)
		command << " --battle-sim-generate-map --battle-sim-generated-mode " << shellQuote(options.generatedMode);
	else
		command << " --testmap " << shellQuote(shard.map);

	for(const auto & arg : options.extraClientArgs)
		command << ' ' << shellQuote(arg);

	command << " > " << shellQuote(shard.logPath.string()) << " 2>&1";
	return command.str();
}

int decodeSystemResult(int result)
{
	if(result == -1)
		return -1;
#ifdef _WIN32
	return result;
#else
	if(WIFEXITED(result))
		return WEXITSTATUS(result);
	if(WIFSIGNALED(result))
		return 128 + WTERMSIG(result);
	return result;
#endif
}

uint64_t countRows(const std::filesystem::path & path)
{
	std::ifstream input(path);
	if(!input)
		return 0;

	uint64_t rows = 0;
	std::string line;
	while(std::getline(input, line))
		++rows;
	return rows;
}

bool runShard(const Options & options, const Shard & shard, uint64_t shardCount)
{
	const auto command = makeCommand(options, shard, shardCount);
	{
		std::lock_guard<std::mutex> lock(outputMutex);
		std::cout << "shard " << shard.index << ": " << shard.battles << " battles, map "
			<< shard.map << ", output " << shard.outputPath << '\n';
		if(options.dryRun)
			std::cout << command << '\n';
	}

	if(options.dryRun)
		return true;

	const auto started = std::chrono::steady_clock::now();
	const int exitCode = decodeSystemResult(std::system(command.c_str()));
	const auto finished = std::chrono::steady_clock::now();
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(finished - started).count();
	const auto rows = countRows(shard.outputPath);
	const bool ok = rows == shard.battles;

	std::lock_guard<std::mutex> lock(outputMutex);
	if(ok)
	{
		std::cout << "shard " << shard.index << " done: " << rows << " rows in " << seconds << "s\n";
		if(exitCode != 0)
			std::cout << "shard " << shard.index << " completed with client exit " << exitCode << '\n';
	}
	else
		std::cerr << "shard " << shard.index << " failed: exit " << exitCode << ", rows " << rows
			<< "/" << shard.battles << ", log " << shard.logPath << '\n';
	return ok;
}

bool runShards(const Options & options, const std::vector<Shard> & shards)
{
	std::atomic<uint64_t> nextShard = 0;
	std::atomic<bool> allOk = true;
	std::vector<std::thread> workers;
	workers.reserve(options.jobs);

	for(unsigned worker = 0; worker < options.jobs; ++worker)
	{
		workers.emplace_back([&]() {
			while(true)
			{
				const auto index = nextShard.fetch_add(1);
				if(index >= shards.size())
					return;
				if(!runShard(options, shards[index], shards.size()))
					allOk = false;
			}
		});
	}

	for(auto & worker : workers)
		worker.join();

	return allOk;
}

void writeManifest(const Options & options, const std::vector<Shard> & shards)
{
	std::ofstream manifest(options.outputDirectory / "manifest.jsonl", std::ios::out | std::ios::trunc);
	if(!manifest)
		throw std::runtime_error("Unable to write manifest in " + options.outputDirectory.string());

	for(const auto & shard : shards)
	{
		manifest << "{\"shard\":" << shard.index
			<< ",\"battles\":" << shard.battles
			<< ",\"seed\":" << shard.seed
			<< ",\"map\":" << jsonQuote(shard.map)
			<< ",\"output\":" << jsonQuote(shard.outputPath.string())
			<< ",\"log\":" << jsonQuote(shard.logPath.string())
			<< "}\n";
	}
}
}

int main(int argc, char ** argv)
{
	try
	{
		auto options = parseOptions(argc, argv);
		auto maps = readMaps(options);
		validateOptions(options, maps);
		std::filesystem::create_directories(options.outputDirectory);

		auto shards = makeShards(options, maps);
		writeManifest(options, shards);

		std::cout << "running " << options.battles << " battles across " << shards.size()
			<< " shards with " << options.jobs << " jobs\n";
		const bool ok = runShards(options, shards);
		return ok ? 0 : 2;
	}
	catch(const std::exception & error)
	{
		std::cerr << error.what() << '\n';
		std::cerr << "Run with --help for usage.\n";
		return 1;
	}
}
