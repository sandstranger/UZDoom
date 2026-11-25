#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

// The ETC2 transcodes of BC images (bctranscode.cpp) kept so that an image is transcoded once: in RAM (LRU) and on
// disk (the cache folder, <hash>.etc2, written by a thread). Thread safe (the texture loader threads use it).
namespace ETC2Cache
{
	// Whether the cache is on (gl_texture_etc2_cache; the Android launcher: setTextureCacheData)
	bool Enabled();
	// The key of a BC image: its data (the whole mip chain), size, format and levels
	uint64_t Hash(const void *bcData, size_t size, int w, int h, int bcFormat, int mips);
	// The cached transcode of an image (RAM first, then disk - put in RAM then): size bytes copied to out
	bool Get(uint64_t hash, void *out, size_t size);
	// A new transcode: kept in RAM and queued for the disk
	void Put(uint64_t hash, const void *etc2, size_t size);
	// Forced ETC2 for the uncompressed textures on OpenGL ES (the launcher: setForceETC2Textures; on by default)
	bool ForceAll();
	// The ETC2 RGBA8 EAC chain (levels) of a BGRA image: from the cache, or encoded (bctranscode) and cached
	void EncodeBGRA(const uint8_t *bgra, int w, int h, int levels, std::vector<uint8_t> &out);
	// Low memory (any thread): drops the RAM copies, unless another thread is using them right now
	bool TryClearRam();
	// Waits for the queued disk writes and stops the writer (the engine's shutdown)
	void Shutdown();
}
