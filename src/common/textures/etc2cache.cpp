/*
** etc2cache.cpp
** The ETC2 transcodes of the BC images (OpenGL ES without the BC formats): a RAM LRU and a disk cache, so that an
** image is transcoded with etcpak only once. A cache file: EtcCacheHeader, then the data.
**
** The state lives on the heap and is never destroyed: the writer thread may still wait on its condition variable
** when the process exits (destroying it, or a joinable std::thread, at exit is undefined / std::terminate).
*/

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>
#include "etc2cache.h"
#include "bctranscode.h"
#include "c_cvars.h"
#include "i_specialpaths.h"
#include "printf.h"

namespace
{
	// (the loader threads read the settings: atomics the CVARs publish)
	std::atomic<bool> CacheOn{ true };
	std::atomic<int> DiskLimitMB{ 4096 }, RamLimitMB{ sizeof(void *) == 4 ? 16 : 128 };
}

// (testing the paths on a GPU with the BC formats) 0: BC kept where the GPU takes it, 1: always transcoded to ETC2,
// 2: the GLES renderer decodes them (its OpenGL ES 2 path)
CVAR(Int, gl_texture_etc2_transcode, 0, 0)

CUSTOM_CVAR(Bool, gl_texture_etc2_cache, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	CacheOn.store(self, std::memory_order_relaxed);
}
// (MB) the disk cache's limit, the RAM cache's (0: no RAM copies)
CUSTOM_CVAR(Int, gl_texture_etc2_cache_mb, 4096, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 64) self = 64;
	else DiskLimitMB.store(self, std::memory_order_relaxed);
}
CUSTOM_CVAR(Int, gl_texture_etc2_ramcache_mb, sizeof(void *) == 4 ? 16 : 128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0) self = 0;
	else RamLimitMB.store(self, std::memory_order_relaxed);
}

namespace
{
	namespace fs = std::filesystem;

	struct EtcCacheHeader
	{
		char magic[4];		// "UZE2"
		uint32_t version;
		uint64_t hash;
		uint64_t size;		// of the data that follows
	};
	static_assert(sizeof(EtcCacheHeader) == 24, "no padding in the header");
	const uint32_t CacheVersion = 1;
	const size_t MaxQueuedJobs = 64;

	// launcher settings (Android, JNA from its own thread): published atomically, the folder never freed
	std::atomic<int> LauncherEnable{ -1 };	// -1: not given (the CVAR decides)
	std::atomic<bool> ForceETC2{ true };
	std::atomic<const char *> LauncherDir{ nullptr };

	struct RamEntry
	{
		uint64_t hash;
		std::vector<uint8_t> data;
	};

	struct WriteJob
	{
		uint64_t hash;
		std::vector<uint8_t> data;
	};

	struct CacheState
	{
		// RAM
		std::mutex ramMutex;
		std::list<RamEntry> ramList;	// front: most recently used
		std::unordered_map<uint64_t, std::list<RamEntry>::iterator> ramIndex;
		size_t ramBytes = 0;

		// disk: the folder, its size, the write queue
		std::mutex diskMutex;
		std::condition_variable diskCV;
		std::deque<WriteJob> jobs;
		std::thread *writer = nullptr;
		bool diskReady = false, stopping = false;
		std::string dir;
		uint64_t diskBytes = 0;
	};
	CacheState &S = *new CacheState;

	std::string FilePath(const std::string &dir, uint64_t hash)
	{
		char name[32];
		snprintf(name, sizeof(name), "%016llx.etc2", (unsigned long long)hash);
		return dir + "/" + name;
	}

	// The folder, created, and the size of its files; unfinished writes of an earlier run removed (diskMutex held)
	void StartDisk()
	{
		if (S.diskReady)
			return;
		const char *launcherDir = LauncherDir.load(std::memory_order_acquire);
		if (launcherDir != nullptr && *launcherDir)
			S.dir = launcherDir;
		else
			S.dir = (M_GetCachePath(true) + "/etc2cache").GetChars();
		std::error_code ec;
		fs::create_directories(S.dir, ec);
		S.diskBytes = 0;
		for (fs::directory_iterator it(S.dir, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code ec2;
			if (it->path().extension() == ".tmp")
				fs::remove(it->path(), ec2);
			else if (it->path().extension() == ".etc2")
			{
				auto size = it->file_size(ec2);
				if (!ec2) S.diskBytes += size;
			}
		}
		S.diskReady = true;
		Printf("ETC2 texture cache: %s, %llu MB\n", S.dir.c_str(), (unsigned long long)(S.diskBytes >> 20));
	}

	// Over the limit: the least recently used fifth of the files goes (writer thread)
	void EvictDisk()
	{
		struct FileInfo { fs::path path; fs::file_time_type time; uint64_t size; };
		std::vector<FileInfo> files;
		std::string dir;
		{
			std::lock_guard<std::mutex> lock(S.diskMutex);
			if (S.diskBytes <= ((uint64_t)DiskLimitMB.load(std::memory_order_relaxed) << 20))
				return;
			dir = S.dir;
		}
		std::error_code ec;
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		{
			if (it->path().extension() != ".etc2") continue;
			std::error_code ec2, ec3;
			auto time = it->last_write_time(ec2);
			auto size = it->file_size(ec3);
			if (!ec2 && !ec3) files.push_back({ it->path(), time, size });
		}
		if (files.empty()) return;
		const size_t count = std::max<size_t>(1, files.size() / 5);
		std::nth_element(files.begin(), files.begin() + (count - 1), files.end(),
			[](const FileInfo &a, const FileInfo &b) { return a.time < b.time; });
		uint64_t freed = 0;
		for (size_t i = 0; i < count; i++)
		{
			std::error_code ec2;
			if (fs::remove(files[i].path, ec2)) freed += files[i].size;
		}
		std::lock_guard<std::mutex> lock(S.diskMutex);
		S.diskBytes -= std::min(S.diskBytes, freed);
	}

	void WriteFile(const WriteJob &job, const std::string &path)
	{
		const std::string tmp = path + ".tmp";
		FILE *f = fopen(tmp.c_str(), "wb");
		if (f == nullptr) return;
		EtcCacheHeader h;
		memcpy(h.magic, "UZE2", 4);
		h.version = CacheVersion;
		h.hash = job.hash;
		h.size = job.data.size();
		bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(job.data.data(), 1, job.data.size(), f) == job.data.size();
		ok = (fclose(f) == 0) && ok;
		std::error_code ec;
		if (ok) fs::rename(tmp, path, ec);
		if (!ok || ec)
		{
			fs::remove(tmp, ec);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(S.diskMutex);
			S.diskBytes += sizeof(h) + job.data.size();
		}
		EvictDisk();
	}

	void WriterThread()
	{
		std::unique_lock<std::mutex> lock(S.diskMutex);
		for (;;)
		{
			S.diskCV.wait(lock, [] { return S.stopping || !S.jobs.empty(); });
			if (S.jobs.empty())
				return;	// stopping, nothing left to write
			WriteJob job = std::move(S.jobs.front());
			S.jobs.pop_front();
			const std::string path = FilePath(S.dir, job.hash);
			lock.unlock();
			WriteFile(job, path);
			lock.lock();
		}
	}

	void PutRam(uint64_t hash, const void *data, size_t size)
	{
		const size_t limit = (size_t)RamLimitMB.load(std::memory_order_relaxed) << 20;
		if (size == 0 || size > limit) return;
		std::lock_guard<std::mutex> lock(S.ramMutex);
		if (S.ramIndex.count(hash)) return;
		S.ramList.push_front({ hash, std::vector<uint8_t>((const uint8_t *)data, (const uint8_t *)data + size) });
		S.ramIndex[hash] = S.ramList.begin();
		S.ramBytes += size;
		while (S.ramBytes > limit && !S.ramList.empty())
		{
			S.ramBytes -= S.ramList.back().data.size();
			S.ramIndex.erase(S.ramList.back().hash);
			S.ramList.pop_back();
		}
	}
}

bool ETC2Cache::Enabled()
{
	const int launcher = LauncherEnable.load(std::memory_order_acquire);
	return launcher >= 0 ? launcher != 0 : CacheOn.load(std::memory_order_relaxed);
}

uint64_t ETC2Cache::Hash(const void *bcData, size_t size, int w, int h, int bcFormat, int mips)
{
	// FNV-1a over the data, then the description
	const uint8_t *p = (const uint8_t *)bcData;
	uint64_t hash = 14695981039346656037ULL;
	for (size_t i = 0; i < size; i++)
	{
		hash ^= p[i];
		hash *= 1099511628211ULL;
	}
	const int32_t desc[4] = { w, h, bcFormat, mips };
	const uint8_t *d = (const uint8_t *)desc;
	for (size_t i = 0; i < sizeof(desc); i++)
	{
		hash ^= d[i];
		hash *= 1099511628211ULL;
	}
	return hash ^ (uint64_t)size;
}

bool ETC2Cache::Get(uint64_t hash, void *out, size_t size)
{
	{
		std::lock_guard<std::mutex> lock(S.ramMutex);
		auto it = S.ramIndex.find(hash);
		if (it != S.ramIndex.end() && it->second->data.size() == size)
		{
			S.ramList.splice(S.ramList.begin(), S.ramList, it->second);
			memcpy(out, it->second->data.data(), size);
			return true;
		}
	}

	std::string path;
	{
		std::lock_guard<std::mutex> lock(S.diskMutex);
		StartDisk();
		path = FilePath(S.dir, hash);
	}
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr) return false;
	EtcCacheHeader h;
	bool ok = fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.magic, "UZE2", 4) == 0 && h.version == CacheVersion &&
		h.hash == hash && h.size == size && fread(out, 1, size, f) == size;
	fclose(f);
	std::error_code ec;
	if (!ok)
	{
		fs::remove(path, ec);	// broken, or another image's: made again
		return false;
	}
	fs::last_write_time(path, fs::file_time_type::clock::now(), ec);	// (recently used: evicted last)
	PutRam(hash, out, size);
	return true;
}

void ETC2Cache::Put(uint64_t hash, const void *etc2, size_t size)
{
	PutRam(hash, etc2, size);

	std::lock_guard<std::mutex> lock(S.diskMutex);
	StartDisk();
	if (S.stopping || S.jobs.size() >= MaxQueuedJobs)
		return;	// (a full queue: this one is transcoded again next time)
	if (S.writer == nullptr)
		S.writer = new std::thread(WriterThread);	// (never destroyed, see above)
	S.jobs.push_back({ hash, std::vector<uint8_t>((const uint8_t *)etc2, (const uint8_t *)etc2 + size) });
	S.diskCV.notify_one();
}

bool ETC2Cache::ForceAll()
{
	return ForceETC2.load(std::memory_order_relaxed);
}

void ETC2Cache::EncodeBGRA(const uint8_t *bgra, int w, int h, int levels, std::vector<uint8_t> &out)
{
	const size_t size = BCT_ETC2ChainSize(w, h, levels);
	out.resize(size);
	const bool cached = Enabled();
	uint64_t hash = 0;
	if (cached)
	{
		hash = Hash(bgra, (size_t)w * h * 4, w, h, BCT_RGBA8_ETC2_EAC, levels);
		if (Get(hash, out.data(), size))
			return;
	}
	BCT_EncodeBGRAToETC2(bgra, w, h, levels, out.data());
	if (cached)
		Put(hash, out.data(), size);
}

bool ETC2Cache::TryClearRam()
{
	std::unique_lock<std::mutex> lock(S.ramMutex, std::try_to_lock);
	if (!lock.owns_lock() || S.ramList.empty())
		return false;
	S.ramList.clear();
	S.ramIndex.clear();
	S.ramBytes = 0;
	return true;
}

void ETC2Cache::Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(S.diskMutex);
		S.stopping = true;
		S.diskCV.notify_all();
	}
	if (S.writer != nullptr && S.writer->joinable())
		S.writer->join();	// (it writes what is queued first)
}

#ifdef __ANDROID__
// JNA (the launcher's thread): the uncompressed textures forced to ETC2 RGBA8 EAC (a quarter of the memory) on the
// OpenGL ES renderers - the GL renderer and the GLES renderer's GLES 3.0 path - or not. On by default. The BC images
// (DDS) are not concerned: kept as they are where the GPU has their format, transcoded to ETC2 where not.
extern "C" __attribute__((used)) __attribute__((visibility("default")))
void setForceETC2Textures(const int enable)
{
	ForceETC2.store(enable != 0, std::memory_order_relaxed);
}

// JNA (the launcher's thread): the cache on or off, and its folder (null or empty: the engine's cache folder). The
// folder is taken at the first cached texture.
extern "C" __attribute__((used)) __attribute__((visibility("default")))
void setTextureCacheData(const int enableTextureCache, const char *pathToTextureCacheDir)
{
	if (pathToTextureCacheDir != nullptr)
		LauncherDir.store(strdup(pathToTextureCacheDir), std::memory_order_release);	// (never freed)
	LauncherEnable.store(enableTextureCache != 0 ? 1 : 0, std::memory_order_release);
}
#endif
