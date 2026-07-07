set(scriptedAdventureAI_SRCS
		CScriptedAdventureAI.cpp
)

set(scriptedAdventureAI_HEADERS
		CScriptedAdventureAI.h
)

assign_source_group(${scriptedAdventureAI_SRCS} ${scriptedAdventureAI_HEADERS})

add_library(ScriptedAdventureAI OBJECT ${scriptedAdventureAI_SRCS} ${scriptedAdventureAI_HEADERS})
target_compile_definitions(ScriptedAdventureAI PRIVATE VCMI_DLL=1)

target_include_directories(ScriptedAdventureAI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ScriptedAdventureAI PRIVATE vcmiMain Nullkiller2 vcmiLua)

vcmi_set_output_dir(ScriptedAdventureAI "AI")
enable_pch(ScriptedAdventureAI)
