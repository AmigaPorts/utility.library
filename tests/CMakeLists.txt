# SPDX-License-Identifier: MPL-2.0

# OpenLibrary("utility.library") always resolves to the resident copy, so the
# test loads this implementation from LIBS: under a name that cannot collide.
add_utility_library(utility-new.library "utility-new.library")

add_executable(UtilityCheck utilitycheck.c kprint.c)
target_compile_definitions(UtilityCheck PRIVATE EXPECT_LIB_VERSION=${PROJECT_VERSION_MAJOR} EXPECT_LIB_REVISION=${PROJECT_VERSION_MINOR})
target_compile_options(UtilityCheck PRIVATE -noixemul -O2 -Wall)
target_link_options(UtilityCheck PRIVATE -noixemul)
