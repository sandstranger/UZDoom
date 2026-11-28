cmake_minimum_required( VERSION 3.16 )

# AsmJit with only its AArch64 backend, for the arm64 ZScript JIT (src/common/scripting/jit_a64).
# Upstream: https://github.com/asmjit/asmjit, commit c5e044a02494a0f01c2399940b7cd50e59880d67 (2026-11-25).
# Copied: asmjit/{core,arm,axl} without tests, plus the a64.h/arm.h/core.h umbrella headers. The x86 JIT keeps using
# libraries/asmjit (the 2017 API), and only one of the two is ever built.

project( asmjit_a64 CXX )

file( GLOB ASMJIT_A64_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/asmjit/core/*.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/asmjit/arm/*.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/asmjit/axl/*.cpp )

add_library( asmjit_a64 STATIC ${ASMJIT_A64_SOURCES} )

target_include_directories( asmjit_a64 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} )
target_compile_definitions( asmjit_a64 PUBLIC ASMJIT_STATIC ASMJIT_NO_X86 ASMJIT_NO_UJIT )
target_compile_features( asmjit_a64 PUBLIC cxx_std_17 )

# Scripts are compiled when they are first called (and all of them at startup with vm_jit_aot), so the compiler's
# own speed matters; the app builds everything else -Os. The last -O on the command line wins.
target_compile_options( asmjit_a64 PRIVATE -O2 )

if( ANDROID )
	# Assertion messages go to logcat (asmjit/core/debug_utils.cpp)
	target_link_libraries( asmjit_a64 PRIVATE log )
endif()
