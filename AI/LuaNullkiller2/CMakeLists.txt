set(luaNullkiller2_SRCS
		CLuaNullkiller2AI.cpp
)

set(luaNullkiller2_HEADERS
		StdInc.h

		CLuaNullkiller2AI.h
)

assign_source_group(${luaNullkiller2_SRCS} ${luaNullkiller2_HEADERS})

add_library(LuaNullkiller2 OBJECT ${luaNullkiller2_SRCS} ${luaNullkiller2_HEADERS})
target_compile_definitions(LuaNullkiller2 PRIVATE VCMI_DLL=1)

target_include_directories(LuaNullkiller2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(LuaNullkiller2 PRIVATE vcmiMain)

vcmi_set_output_dir(LuaNullkiller2 "AI")
enable_pch(LuaNullkiller2)
