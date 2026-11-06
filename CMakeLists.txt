cmake_minimum_required (VERSION 3.5)
project (reforge CXX)
set (reforge_VERSION_MAJOR 0)
set (reforge_VERSION_MINOR 1)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# Build outputs go to <build dir>/out (the source tree is not written to).
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out/bin)

# Dependency locations. Each one is taken from the CMake variable of the same
# name (-DXSTL_PATH=...), else from the environment variable, else from the
# sibling checkout next to this repository.
function(reforge_resolve_path VAR ENVNAME DEFAULT)
	if("${${VAR}}" STREQUAL "")
		if(NOT "$ENV{${ENVNAME}}" STREQUAL "")
			set(value "$ENV{${ENVNAME}}")
		else()
			set(value "${DEFAULT}")
		endif()
	else()
		set(value "${${VAR}}")
	endif()
	get_filename_component(value "${value}" ABSOLUTE)
	set(${VAR} "${value}" PARENT_SCOPE)
endfunction()

reforge_resolve_path(XSTL_PATH XSTL_PATH "${CMAKE_SOURCE_DIR}/../xStl")
reforge_resolve_path(PELIB_PATH PELIB_PATH "${CMAKE_SOURCE_DIR}/../pe")
reforge_resolve_path(ELFLIB_PATH ELFLIB_PATH "${CMAKE_SOURCE_DIR}/../elf")
reforge_resolve_path(DISMOUNT_PATH DISMOUNT_PATH "${CMAKE_SOURCE_DIR}/../dismount")

message(STATUS "reforge: xStl     ${XSTL_PATH}")
message(STATUS "reforge: pe       ${PELIB_PATH}")
message(STATUS "reforge: elf      ${ELFLIB_PATH}")
message(STATUS "reforge: dismount ${DISMOUNT_PATH}")

# Same definitions as the sibling libraries, so that xStl headers match the
# built archives.
add_definitions(-DXSTL_UNICODE)
add_definitions(-D_UNICODE)
if(UNIX)
	add_definitions(-DLINUX)
endif()
if(WIN32)
	add_definitions(-DWIN32)
endif()

include_directories(${CMAKE_SOURCE_DIR}/Include)
if(NOT EXISTS "${XSTL_PATH}/Include/xStl/types.h")
	message(FATAL_ERROR "xStl headers not found in '${XSTL_PATH}'. Set XSTL_PATH to the xStl checkout.")
endif()
include_directories(${XSTL_PATH}/Include)

# xStl is the host infrastructure of every stage, so its archive is required.
find_library(LIBXSTL_STATIC NAMES xStlcm_static HINTS ${XSTL_PATH}/out/lib NO_DEFAULT_PATH)
if(NOT LIBXSTL_STATIC)
	message(FATAL_ERROR "xStl static library (xStlcm_static) not found in ${XSTL_PATH}/out/lib. Build xStl first.")
endif()

# The sibling libraries are used when they are built; otherwise they are left
# out and the stages that need them report that they are missing.
set(REFORGE_LINK_LIBS)
macro(reforge_use_library VAR LIBNAME ROOT)
	if(EXISTS "${ROOT}/Include")
		include_directories(${ROOT}/Include)
	endif()
	find_library(${VAR} NAMES ${LIBNAME} HINTS ${ROOT}/out/lib NO_DEFAULT_PATH)
	if(${VAR})
		message(STATUS "reforge: linking ${${VAR}}")
		list(APPEND REFORGE_LINK_LIBS ${${VAR}})
	else()
		message(STATUS "reforge: ${LIBNAME} not found in ${ROOT}/out/lib, not linked")
	endif()
endmacro()

reforge_use_library(LIB_DISMOUNT dismount_static ${DISMOUNT_PATH})
reforge_use_library(LIB_PE pe_static ${PELIB_PATH})
reforge_use_library(LIB_ELF elf_static ${ELFLIB_PATH})
list(APPEND REFORGE_LINK_LIBS ${LIBXSTL_STATIC})

find_package(Threads REQUIRED)

# Driver stages shared by the executable and the unit tests.
add_library(reforge_core STATIC
	Source/reforge/Amd64Lowerer.cpp
	Source/reforge/BlockLinker.cpp
	Source/reforge/Diagnostics.cpp
	Source/reforge/FlowRecovery.cpp
	Source/reforge/HostLinker.cpp
	Source/reforge/MetaBuilder.cpp
	Source/reforge/ObjectEmitter.cpp
	Source/reforge/PeLoader.cpp
	Source/reforge/Translator.cpp
)

add_executable(reforge
	Source/reforge/main.cpp
	Source/reforge/Options.cpp
)
target_link_libraries(reforge reforge_core ${REFORGE_LINK_LIBS} Threads::Threads)
# The translator links every output against the runtime archive.
add_dependencies(reforge reforge_rt)

# Runtime linked into every translated executable (WP13): libreforge_rt.a holds
# the dispatch loop, guest memory, the import adapters and main(). Program output
# goes through the xStl standard streams only.
add_library(reforge_rt STATIC
	Runtime/Source/Adapter.cpp
	Runtime/Source/AdapterRegistry.cpp
	Runtime/Source/FaultLine.cpp
	Runtime/Source/GuestMemory.cpp
	Runtime/Source/Program.cpp
	Runtime/Source/Runtime.cpp
	Runtime/Source/RuntimeMain.cpp
	Runtime/Source/StandardStreams.cpp
	Runtime/Source/adapters/Kernel32ExitProcess.cpp
	Runtime/Source/adapters/MsvcrtPrintf.cpp
)
target_include_directories(reforge_rt PUBLIC
	${CMAKE_SOURCE_DIR}/Runtime/Include ${CMAKE_SOURCE_DIR}/Include ${XSTL_PATH}/Include)
target_link_libraries(reforge_rt ${LIBXSTL_STATIC} Threads::Threads)

# What the host link step (HostLinker.cpp) needs: the host C++ driver used as
# linker, the host assembler (--object-writer asm) and the archives that every
# translated executable is linked with.
target_compile_definitions(reforge_core PRIVATE
	"REFORGE_HOST_CXX=\"${CMAKE_CXX_COMPILER}\""
	"REFORGE_HOST_AS=\"as\""
	"REFORGE_RT_LIB=\"$<TARGET_FILE:reforge_rt>\""
	"REFORGE_XSTL_LIB=\"${LIBXSTL_STATIC}\"")
add_dependencies(reforge_core reforge_rt)

# Unit tests use the xStl test harness (tests.cpp provides main()). Each test
# file is one line in test_reforge's source list below.
set(REFORGE_FIXTURES_DEFAULT "${CMAKE_BINARY_DIR}/out/fixtures"
	CACHE PATH "Directory of the PE fixtures written by scripts/build_fixtures.sh")
add_executable(test_reforge
	${XSTL_PATH}/tests/tests.cpp
	tests/TestBlockLinker.cpp
	tests/TestFlowRecovery.cpp
	tests/TestObjectEmitter.cpp
	tests/TestPeLoader.cpp
	tests/TestRuntimeAdapters.cpp
)
target_include_directories(test_reforge PRIVATE ${XSTL_PATH}/tests)
target_compile_definitions(test_reforge PRIVATE
	"REFORGE_FIXTURE_DIR=\"${REFORGE_FIXTURES_DEFAULT}\"")
target_link_libraries(test_reforge reforge_core reforge_rt ${REFORGE_LINK_LIBS} Threads::Threads)
enable_testing()
add_test(NAME test_reforge COMMAND test_reforge)

# The PE fixtures (scripts/build_fixtures.sh needs llvm-mc, ld.lld and python3).
# Without them the target is not defined and the fixture tests report the
# missing directory.
find_program(REFORGE_LLVM_MC llvm-mc)
find_program(REFORGE_LLD ld.lld)
find_program(REFORGE_PYTHON python3)
if(REFORGE_LLVM_MC AND REFORGE_LLD AND REFORGE_PYTHON)
	add_custom_target(reforge_fixtures
		COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_fixtures.sh ${REFORGE_FIXTURES_DEFAULT}
		COMMENT "Building the RISC-V64 PE fixtures"
		VERBATIM)
	add_dependencies(test_reforge reforge_fixtures)
else()
	message(STATUS "reforge: llvm-mc/ld.lld/python3 not found, fixtures are not built")
endif()
