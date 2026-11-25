make_release_only()

# etcpak (submodule libraries/etcpak, https://github.com/wolfpld/etcpak, BSD 3-clause): only the ETC2 encoder
# (ProcessRGB) and bcdec (BC1-7 decoder, MIT / public domain) - its own CMakeLists builds the command line tool.
# Used to transcode the BC compressed textures (DDS) for GPUs without the BC formats (OpenGL ES: S3TC / BPTC are rare
# there, ETC2 is core in OpenGL ES 3.0).
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
