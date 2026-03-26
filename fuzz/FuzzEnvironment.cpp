/*
 * FuzzEnvironment.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "FuzzEnvironment.h"

#include "../lib/GameLibrary.h"

#include <memory>
#include <mutex>

namespace fuzzing
{
namespace
{
std::once_flag & engineInitOnce()
{
	static std::once_flag flag;
	return flag;
}

std::unique_ptr<GameLibrary> & gameLibraryHolder()
{
	static std::unique_ptr<GameLibrary> holder;
	return holder;
}
}

void initializeEngine()
{
	std::call_once(engineInitOnce(), []()
	{
		auto & libraryHolder = gameLibraryHolder();
		libraryHolder = std::make_unique<GameLibrary>();
		LIBRARY = libraryHolder.get();
		LIBRARY->initializeFilesystem(false);
		LIBRARY->initializeLibrary();
	});
}
}
