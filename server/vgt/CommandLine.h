/*
 * CommandLine.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <optional>

namespace boost::program_options
{
class options_description;
class variables_map;
}

namespace vgt
{

#if defined(VCMI_ENABLE_VGT)
void addCommandLineOptions(boost::program_options::options_description & options);
void configureLogLevel(const boost::program_options::variables_map & options);
std::optional<int> handleCommandLine(const boost::program_options::variables_map & options);
#else
inline void addCommandLineOptions(boost::program_options::options_description &)
{
}

inline void configureLogLevel(const boost::program_options::variables_map &)
{
}

inline std::optional<int> handleCommandLine(const boost::program_options::variables_map &)
{
	return std::nullopt;
}
#endif

}
