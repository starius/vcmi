/*
 * RmgReproFuzzer.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "FuzzEnvironment.h"
#include "RmgFuzzSpec.h"

#include "../lib/filesystem/CMemoryBuffer.h"
#include "../lib/mapping/MapFormatJson.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace
{
bool strictThreadInvariantMode()
{
	static const bool enabled = []()
	{
		if(const char * value = std::getenv("VCMI_FUZZ_CHECK_THREAD_INVARIANCE"))
		{
			const std::string lowered = boost::algorithm::to_lower_copy(std::string(value));
			return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
		}
		return false;
	}();

	return enabled;
}

std::vector<ui8> serializeMapState(std::unique_ptr<CMap> map)
{
	CMemoryBuffer output;
	CMapSaverJson saver(&output);
	saver.saveMap(map);
	return output.getBuffer();
}

std::optional<std::vector<ui8>> generateSerializedMapState(const fuzzing::RmgGenerationSpec & spec, int parallelism)
{
	try
	{
		auto map = fuzzing::generateMapWithParallelism(spec, parallelism);
		return serializeMapState(std::move(map));
	}
	catch(const std::exception &)
	{
		return std::nullopt;
	}
}

std::optional<std::string> parseArgValue(const std::string & arg, const std::string & prefix)
{
	if(!boost::algorithm::starts_with(arg, prefix))
		return std::nullopt;

	return arg.substr(prefix.size());
}

std::string readTextFile(const boost::filesystem::path & path)
{
	std::ifstream input(path.string(), std::ios::binary);
	if(!input)
		throw std::runtime_error("Failed to open text seed file: " + path.string());

	return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> readBinaryFile(const boost::filesystem::path & path)
{
	std::ifstream input(path.string(), std::ios::binary);
	if(!input)
		throw std::runtime_error("Failed to open binary seed file: " + path.string());

	return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void writeTextFile(const boost::filesystem::path & path, const std::string & content)
{
	std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
	if(!output)
		throw std::runtime_error("Failed to write text seed file: " + path.string());

	output << content;
}

void writeBinaryFile(const boost::filesystem::path & path, const std::vector<uint8_t> & content)
{
	std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
	if(!output)
		throw std::runtime_error("Failed to write binary seed file: " + path.string());

	if(!content.empty())
	{
		output.write(reinterpret_cast<const char *>(content.data()), static_cast<std::streamsize>(content.size()));
		if(!output)
			throw std::runtime_error("Failed to write binary seed file: " + path.string());
	}
}

std::vector<boost::filesystem::path> listRegularFiles(const boost::filesystem::path & directory)
{
	std::vector<boost::filesystem::path> result;
	if(!boost::filesystem::exists(directory))
		throw std::runtime_error("Directory does not exist: " + directory.string());
	if(!boost::filesystem::is_directory(directory))
		throw std::runtime_error("Expected directory path: " + directory.string());

	for(const auto & entry : boost::filesystem::directory_iterator(directory))
	{
		if(boost::filesystem::is_regular_file(entry.path()))
			result.push_back(entry.path());
	}

	std::sort(result.begin(), result.end());
	return result;
}

void prepareTextCorpus(const boost::filesystem::path & textCorpusDir, const boost::filesystem::path & binaryCorpusDir)
{
	boost::filesystem::create_directories(binaryCorpusDir);

	const auto textFiles = listRegularFiles(textCorpusDir);
	size_t converted = 0;
	for(const auto & textFile : textFiles)
	{
		if(!boost::algorithm::iends_with(textFile.filename().string(), ".txt"))
			continue;

		const auto spec = fuzzing::parseRmgGenerationSpecText(readTextFile(textFile));
		const auto binary = fuzzing::encodeRmgGenerationSpec(spec);

		boost::filesystem::path outputName = textFile.filename();
		outputName = outputName.stem();
		const auto outputPath = binaryCorpusDir / outputName;
		writeBinaryFile(outputPath, binary);
		++converted;
	}

	std::cout << "Prepared " << converted << " text seed(s) into " << binaryCorpusDir.string() << '\n';
}

void decodeArtifact(const boost::filesystem::path & binaryArtifactPath, const boost::filesystem::path & textOutputPath)
{
	const auto binaryData = readBinaryFile(binaryArtifactPath);
	const auto spec = fuzzing::decodeRmgGenerationSpec(binaryData.data(), binaryData.size());
	writeTextFile(textOutputPath, fuzzing::serializeRmgGenerationSpecText(spec));

	std::cout << "Decoded artifact into " << textOutputPath.string() << '\n';
}

void decodeCorpus(const boost::filesystem::path & binaryCorpusDir, const boost::filesystem::path & textOutputDir)
{
	boost::filesystem::create_directories(textOutputDir);

	const auto binaryFiles = listRegularFiles(binaryCorpusDir);
	size_t decoded = 0;
	for(const auto & binaryFile : binaryFiles)
	{
		const auto binaryData = readBinaryFile(binaryFile);
		const auto spec = fuzzing::decodeRmgGenerationSpec(binaryData.data(), binaryData.size());
		const auto outputPath = textOutputDir / (binaryFile.filename().string() + ".txt");
		writeTextFile(outputPath, fuzzing::serializeRmgGenerationSpecText(spec));
		++decoded;
	}

	std::cout << "Decoded " << decoded << " binary seed(s) into " << textOutputDir.string() << '\n';
}
}

extern "C" int LLVMFuzzerInitialize(int * argc, char *** argv)
{
	std::optional<std::string> prepareTextCorpusDir;
	std::optional<std::string> binaryCorpusOut;
	std::optional<std::string> decodeArtifactPath;
	std::optional<std::string> decodeCorpusDir;
	std::optional<std::string> textOutPath;
	std::optional<std::string> textOutDir;

	for(int i = 1; i < *argc; ++i)
	{
		const std::string arg = (*argv)[i];
		if(const auto value = parseArgValue(arg, "--prepare-text-corpus="))
			prepareTextCorpusDir = *value;
		else if(const auto value = parseArgValue(arg, "--binary-corpus-out="))
			binaryCorpusOut = *value;
		else if(const auto value = parseArgValue(arg, "--decode-artifact="))
			decodeArtifactPath = *value;
		else if(const auto value = parseArgValue(arg, "--decode-corpus="))
			decodeCorpusDir = *value;
		else if(const auto value = parseArgValue(arg, "--text-out="))
			textOutPath = *value;
		else if(const auto value = parseArgValue(arg, "--text-out-dir="))
			textOutDir = *value;
	}

	try
	{
		if(prepareTextCorpusDir)
		{
			const boost::filesystem::path binaryOut = binaryCorpusOut
				? boost::filesystem::path(*binaryCorpusOut)
				: boost::filesystem::path(*prepareTextCorpusDir + ".bin");
			prepareTextCorpus(boost::filesystem::path(*prepareTextCorpusDir), binaryOut);
			std::exit(EXIT_SUCCESS);
		}

		if(decodeArtifactPath)
		{
			const boost::filesystem::path textOut = textOutPath
				? boost::filesystem::path(*textOutPath)
				: boost::filesystem::path(*decodeArtifactPath + ".txt");
			decodeArtifact(boost::filesystem::path(*decodeArtifactPath), textOut);
			std::exit(EXIT_SUCCESS);
		}

		if(decodeCorpusDir)
		{
			const boost::filesystem::path outputDir = textOutDir
				? boost::filesystem::path(*textOutDir)
				: boost::filesystem::path(*decodeCorpusDir + ".txt");
			decodeCorpus(boost::filesystem::path(*decodeCorpusDir), outputDir);
			std::exit(EXIT_SUCCESS);
		}
	}
	catch(const std::exception & e)
	{
		std::cerr << "Fuzzer corpus utility failed: " << e.what() << '\n';
		std::exit(EXIT_FAILURE);
	}

	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size)
{
	if(size == 0)
		return 0;

	fuzzing::initializeEngine();

	const auto spec = fuzzing::decodeRmgGenerationSpec(data, size);

	const auto baselineState = generateSerializedMapState(spec, spec.parallelism);
	if(!baselineState)
		return 0;

	const auto replayState = generateSerializedMapState(spec, spec.parallelism);
	if(!replayState)
		return 0;

	if(*baselineState != *replayState)
		std::abort();

	if(!spec.singleThread && strictThreadInvariantMode())
	{
		const int alternateParallelism = spec.parallelism == 1 ? 4 : 1;
		const auto alternateState = generateSerializedMapState(spec, alternateParallelism);
		if(alternateState && *baselineState != *alternateState)
			std::abort();
	}

	return 0;
}
