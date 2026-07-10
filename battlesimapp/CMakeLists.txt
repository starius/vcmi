set(battlesimapp_SRCS
	EntryPoint.cpp
)

assign_source_group(${battlesimapp_SRCS})

add_executable(vcmibattlesim ${battlesimapp_SRCS})
vcmi_set_output_dir(vcmibattlesim "")

install(TARGETS vcmibattlesim DESTINATION ${BIN_DIR})
