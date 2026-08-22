make_release_only()

if (MSVC)
	set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4244" )
endif()

add_library( miniz-legacy miniz.c )
target_compile_definitions( miniz-legacy PRIVATE -DMINIZ_NO_STDIO )
target_include_directories( miniz-legacy INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>" )