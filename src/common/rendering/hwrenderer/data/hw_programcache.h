#pragma once

// Persistent cache of linked GPU program binaries (glGetProgramBinary blobs), shared by the
// OpenGL and the OpenGL ES renderers.
//
// Layout on disk:  <root>/<namespace>/<driver hash>/<key>.bin
//   - one small file per program (no monolithic file that is rewritten on every store),
//   - each file carries a versioned header, the key and a CRC32 of the payload; anything that
//     does not validate is deleted and the program is recompiled from source,
//   - files are written to <key>.tmp and renamed, so a crash never leaves a half-written entry
//     under the final name,
//   - when the driver identity changes (driver update, other GPU), the directories of the old
//     driver are deleted the first time the namespace is used.
// Namespaces keep binaries of different renderers / shading-language paths apart
// ("gl", "gl-spirv", "gles2", "gles3", ...).
// A small RAM LRU (bounded by bytes and entries) keeps recently used binaries so that
// recreating a program during the session does not touch the disk.

#include <stdint.h>
#include "tarray.h"
#include "zstring.h"

namespace ProgramBinaryCache
{
	// Builds the lookup key. 'driver' must identify the driver (vendor/renderer/version),
	// 'pipeline' identifies how sources become GPU code (e.g. a source-conversion version).
	FString MakeKey(const char *ns, const char *driver, const char *pipeline, const FString &vertex, const FString &fragment);

	bool Load(const char *ns, const char *driver, const FString &key, uint32_t &format, TArray<uint8_t> &data);
	void Store(const char *ns, const char *driver, const FString &key, uint32_t format, const TArray<uint8_t> &data);
	// The driver refused a binary that came out of the cache: forget it everywhere.
	void Reject(const char *ns, const char *driver, const FString &key);

	// Drops the RAM copies (all of them, or down to half the budget).
	void TrimMemory(bool all);
	// Memory pressure signalled from any thread (SDL_APP_LOWMEMORY); the RAM copies are
	// dropped on the render thread by ProcessPendingTrim (called once per frame).
	void RequestTrim();
	void ProcessPendingTrim();
	bool TrimRequested();
	void PrintStats();
}

extern "C" void UZDoom_SetShaderCacheDirectory(const char *path);
