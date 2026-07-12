/*
 * NativeTrace.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "NativeTrace.h"

#include <cstdlib>
#include <fstream>

namespace NK2AI
{

namespace
{
const char * TRACE_PATH_ENV = "VCMI_NK2_NATIVE_TRACE";

void addCompareField(JsonNode & fields, const std::string & field)
{
	fields.Vector().push_back(JsonNode(field));
}
}

NativeTrace::NativeTrace()
{
	const char * tracePath = std::getenv(TRACE_PATH_ENV);
	if(!tracePath || tracePath[0] == '\0')
		return;

	outputPath = tracePath;
	document.setType(JsonNode::JsonType::DATA_STRUCT);
	document["schema"].String() = "LuaNullkiller2.nativeDecisionTrace.v1";
	document["source"].String() = "Nullkiller2";
	document["compareMode"].String() = "exact";
	document["compare"].setType(JsonNode::JsonType::DATA_VECTOR);
	addCompareField(document["compare"], "status");
	addCompareField(document["compare"], "selection");
	addCompareField(document["compare"], "commandJournal");
	document["decisions"].setType(JsonNode::JsonType::DATA_VECTOR);

	std::lock_guard lock(mutex);
	flushLocked();
}

bool NativeTrace::enabled() const
{
	return !outputPath.empty();
}

void NativeTrace::recordDecision(const std::string & id, const std::string & functionName, JsonNode input, JsonNode nativeOutput)
{
	if(!enabled())
		return;

	JsonNode decision;
	decision.setType(JsonNode::JsonType::DATA_STRUCT);
	decision["id"].String() = id;
	decision["function"].String() = functionName;
	decision["input"] = std::move(input);
	decision["native"] = std::move(nativeOutput);

	std::lock_guard lock(mutex);
	document["decisions"].Vector().push_back(std::move(decision));
	flushLocked();
}

void NativeTrace::flushLocked() const
{
	std::ofstream file(outputPath, std::ofstream::out | std::ofstream::trunc);
	if(!file)
	{
		logAi->warn("Nullkiller2 native trace could not write to %s", outputPath);
		return;
	}

	file << document.toString();
}

}
