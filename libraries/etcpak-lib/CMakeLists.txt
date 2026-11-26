make_release_only()

# etcpak (submodule libraries/etcpak, https://github.com/wolfpld/etcpak, BSD 3-clause): only the ETC2 encoder
# (ProcessRGB) and bcdec (BC1-7 decoder, MIT / public domain) - its own CMakeLists builds the command line tool.
# Used to transcode the BC compressed textures (DDS) for GPUs without the BC formats (OpenGL ES: S3TC / BPTC are rare
# there, ETC2 is core in OpenGL ES 3.0).

# A parent project that builds etcpak itself (the Android app: one etcpak for all its engines) is used as is: its
# library is linked and its headers come along. Its target may set only directory include paths: the one of them with
# ProcessRGB.hpp becomes the target's interface include path then.
if( TARGET etcpak )
	get_target_property( ETCPAK_INTERFACE_INCLUDES etcpak INTERFACE_INCLUDE_DIRECTORIES )
	if( NOT ETCPAK_INTERFACE_INCLUDES )
		get_target_property( ETCPAK_INCLUDES etcpak INCLUDE_DIRECTORIES )
		unset( ETCPAK_INCLUDE_DIR )
		foreach( ETCPAK_INCLUDE IN LISTS ETCPAK_INCLUDES )
			if( EXISTS "${ETCPAK_INCLUDE}/ProcessRGB.hpp" )
				set( ETCPAK_INCLUDE_DIR "${ETCPAK_INCLUDE}" )
				break()
			endif()
		endforeach()
		if( NOT ETCPAK_INCLUDE_DIR )
			message( FATAL_ERROR "etcpak target of the parent project: no include path with ProcessRGB.hpp" )
		endif()
		target_include_directories( etcpak INTERFACE "$<BUILD_INTERFACE:${ETCPAK_INCLUDE_DIR}>" )
	endif()
	message( STATUS "etcpak: the parent project's target" )
	return()
endif()

set( ETCPAK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../etcpak" )
add_library( etcpak STATIC
	"${ETCPAK_DIR}/ProcessRGB.cpp"
	"${ETCPAK_DIR}/Dither.cpp"
	"${ETCPAK_DIR}/Tables.cpp"
	"${ETCPAK_DIR}/bcdec.c" )
target_include_directories( etcpak INTERFACE "$<BUILD_INTERFACE:${ETCPAK_DIR}>" )
if( MSVC )
	target_compile_options( etcpak PRIVATE /wd4244 /wd4267 )
endif()
