# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.14.0)
project(utility.library LANGUAGES C VERSION 40.1)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CPM-init)

get_verstring(VERSTRING)

set(UTILITY_CPU "68000" CACHE STRING "Minimum target CPU")
set_property(CACHE UTILITY_CPU PROPERTY STRINGS 68000 68020 68030 68040 68060)

if(NOT UTILITY_CPU MATCHES "^(68000|68020|68030|68040|68060)$")
    message(FATAL_ERROR "Unknown UTILITY_CPU: ${UTILITY_CPU}")
endif()

option(UTILITY_BUILD_TESTS "Build the differential test program and its side-by-side library" OFF)

# Set default install prefix to ./install subdirectory
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
	set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

sfdc(sfd/utility_lib.sfd)

# Builds the module as TARGET, announcing itself to Exec as NAME. A disk
# library is only found if its Resident name equals its file name.
function(add_utility_library TARGET NAME)
	add_executable(${TARGET}
			${PROJECT_SOURCE_DIR}/src/main.c
			${PROJECT_SOURCE_DIR}/src/init.c
			${PROJECT_SOURCE_DIR}/src/vectors.c
			${PROJECT_SOURCE_DIR}/src/entry.c
			${PROJECT_SOURCE_DIR}/src/tags.c
			${PROJECT_SOURCE_DIR}/src/date.c
			${PROJECT_SOURCE_DIR}/src/math.c
			${PROJECT_SOURCE_DIR}/src/string.c
			${PROJECT_SOURCE_DIR}/src/pack.c
			${PROJECT_SOURCE_DIR}/src/namedobject.c
	)

	target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/include)
	target_compile_definitions(${TARGET} PRIVATE VERSION_STRING="${VERSTRING}" LIBRARY_VERSION=${PROJECT_VERSION_MAJOR} LIBRARY_REVISION=${PROJECT_VERSION_MINOR} LIBRARY_NAME="${NAME}")
	set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "${PROJECT_SOURCE_DIR}/ldscript.lds")

	# Retain the complete ABI from the Resident while dropping unreachable sections.
	target_compile_options(${TARGET} PRIVATE -ffunction-sections -fdata-sections)
	target_link_options(${TARGET} PRIVATE -Wl,--gc-sections -Wl,-u,_Init -Wl,-u,_utilityFunctions -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.map)
	target_link_options(${TARGET} PRIVATE -nostdlib -nostartfiles -Wall -Wextra -s -Wl,-T${PROJECT_SOURCE_DIR}/ldscript.lds)
	target_compile_options(${TARGET} PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -m${UTILITY_CPU} -fomit-frame-pointer -ffreestanding -fno-builtin -fno-common -Wall -Wextra>)
	check_rom(${TARGET} ${TARGET})
endfunction()

add_utility_library(utility.library "${PROJECT_NAME}")
add_dependencies(utility.library utility)
bin_to_header(utility.library)

# Set up information for Aminet
add_project_info(
		"utility.library open source recreation"
		"simond@irrelevant.org (Simon Dick)"
		"util/libs"
		"m68k-amigaos"
)

add_aminet_readme_generator(utility.library "utility.library.readme")

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/sfd/utility_lib.sfd DESTINATION Developer/sfd)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/ DESTINATION Developer/include)
install(FILES LICENSE HEADERS.md DESTINATION Developer)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/utility.library DESTINATION Libs)

if(UTILITY_BUILD_TESTS)
	add_subdirectory(tests)
endif()
