/*
** hw_programcache.cpp
** Persistent, per-driver cache of linked GPU program binaries (see hw_programcache.h).
*/

#include "hw_programcache.h"

#include <stdio.h>
#include <string.h>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <filesystem>
#include <system_error>
#include <atomic>

#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "i_specialpaths.h"
#include "md5.h"
#include "m_crc32.h"
#include "printf.h"

CVAR(Bool, r_programcache, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// RAM copies of binaries kept for the session. Binaries are only needed until glProgramBinary
// has consumed them, so this is deliberately small, and smaller on 32-bit devices.
#if defined(__arm__) || defined(__i386__)
CVAR(Int, r_programcache_ram_kb, 2048, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#else
CVAR(Int, r_programcache_ram_kb, 8192, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#endif

namespace
{
	constexpr char EntryMagic[4] = { 'U', 'Z', 'P', 'B' };
	constexpr uint32_t EntryVersion = 1;
	constexpr uint32_t MaxBinarySize = 16u << 20;		// anything bigger is treated as corruption
	constexpr size_t MaxRamEntries = 256;

	struct EntryHeader
	{
		char magic[4];
		uint32_t version;
		uint32_t format;
		uint32_t size;
		uint32_t crc;
		char key[32];
	};
	static_assert(sizeof(EntryHeader) == 52, "unexpected header packing");

	struct RamEntry
	{
		std::string id;			// ns/driver/key
		uint32_t format;
		TArray<uint8_t> data;
	};

	struct Stats
	{
		unsigned ramHits = 0, diskHits = 0, misses = 0, stores = 0, rejects = 0, corrupt = 0, writeFailures = 0, evictions = 0;
		uint64_t bytesRead = 0, bytesWritten = 0, bytesEvicted = 0, trimmedBytes = 0, peakRam = 0;
	};

	std::string RootOverride;
	std::list<RamEntry> Lru;		// front = most recently used
	std::unordered_map<std::string, std::list<RamEntry>::iterator> LruIndex;
	size_t LruBytes = 0;
	Stats Counters;
	std::unordered_map<std::string, std::string> PreparedDirs;	// ns/driver -> directory (old drivers already pruned)
	bool LegacyFilesRemoved = false;
	std::atomic<bool> TrimPending{ false };

	FString HexDigest(const uint8_t *digest, int bytes)
	{
		static const char hex[] = "0123456789abcdef";
		FString out;
		for (int i = 0; i < bytes; i++)
		{
			char c[3] = { hex[digest[i] >> 4], hex[digest[i] & 15], 0 };
			out += c;
		}
		return out;
	}

	FString Md5Hex(std::initializer_list<std::string_view> parts)
	{
		MD5Context md5;
		for (auto p : parts)
		{
			// Length prefixes keep ("ab","c") and ("a","bc") apart.
			uint32_t len = (uint32_t)p.size();
			md5.Update((const uint8_t *)&len, sizeof(len));
			md5.Update((const uint8_t *)p.data(), (unsigned)p.size());
		}
		uint8_t digest[16];
		md5.Final(digest);
		return HexDigest(digest, 16);
	}

	std::string RootDirectory()
	{
		if (!RootOverride.empty())
			return RootOverride;
		FString path = M_GetCachePath(true);
		path << "/programcache";
		return path.GetChars();
	}

	void RemoveLegacyFiles()
	{
		if (LegacyFilesRemoved)
			return;
		LegacyFilesRemoved = true;
		// The previous format: one file holding every binary, rewritten on every store.
		FString base = M_GetCachePath(false);
		for (const char *name : { "/glshadercache", "/spirv_glshadercache", "/glesshadercache" })
		{
			FString path = base + name;
			std::error_code ec;
			std::filesystem::remove(std::filesystem::path(path.GetChars()), ec);
		}
	}

	// Returns <root>/<ns>/<driverhash>, creating it and deleting the directories of other drivers.
	const std::string &DriverDirectory(const char *ns, const char *driver)
	{
		std::string id = std::string(ns) + "/" + driver;
		auto found = PreparedDirs.find(id);
		if (found != PreparedDirs.end())
			return found->second;

		RemoveLegacyFiles();

		FString driverHash = Md5Hex({ driver }).Left(16);
		std::filesystem::path nsDir = std::filesystem::path(RootDirectory()) / ns;
		std::filesystem::path dir = nsDir / driverHash.GetChars();
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);

		// Binaries of another driver can never be loaded again (glProgramBinary would refuse them).
		for (std::filesystem::directory_iterator it(nsDir, ec), end; !ec && it != end; it.increment(ec))
		{
			if (it->path().filename() != dir.filename())
			{
				std::error_code ec2;
				std::filesystem::remove_all(it->path(), ec2);
			}
		}
		return PreparedDirs[id] = dir.string();
	}

	std::string EntryPath(const std::string &dir, const FString &key, const char *ext)
	{
		return dir + "/" + key.GetChars() + ext;
	}

	void LruErase(const std::string &id)
	{
		auto it = LruIndex.find(id);
		if (it == LruIndex.end())
			return;
		LruBytes -= it->second->data.Size();
		Lru.erase(it->second);
		LruIndex.erase(it);
	}

	void LruEnforce(size_t budget)
	{
		while (!Lru.empty() && (LruBytes > budget || Lru.size() > MaxRamEntries))
		{
			Counters.evictions++;
			Counters.bytesEvicted += Lru.back().data.Size();
			LruBytes -= Lru.back().data.Size();
			LruIndex.erase(Lru.back().id);
			Lru.pop_back();
		}
	}

	size_t RamBudget()
	{
		int kb = r_programcache_ram_kb;
		return kb > 0 ? (size_t)kb * 1024 : 0;
	}

	void LruInsert(const std::string &id, uint32_t format, const TArray<uint8_t> &data)
	{
		LruErase(id);
		size_t budget = RamBudget();
		if (data.Size() > budget / 2)	// would evict half the cache for one entry; disk has it
			return;
		Lru.push_front({ id, format, data });
		LruIndex[id] = Lru.begin();
		LruBytes += data.Size();
		if (LruBytes > Counters.peakRam) Counters.peakRam = LruBytes;
		LruEnforce(budget);
	}

	bool ReadEntry(const std::string &path, const FString &key, uint32_t &format, TArray<uint8_t> &data, bool &corrupt)
	{
		corrupt = false;
		FILE *f = fopen(path.c_str(), "rb");
		if (!f)
			return false;

		EntryHeader h;
		bool ok = fread(&h, sizeof(h), 1, f) == 1 &&
			memcmp(h.magic, EntryMagic, 4) == 0 &&
			h.version == EntryVersion &&
			memcmp(h.key, key.GetChars(), 32) == 0 &&
			h.size > 0 && h.size <= MaxBinarySize;
		if (ok)
		{
			data.Resize(h.size);
			ok = fread(data.Data(), 1, h.size, f) == h.size &&
				fgetc(f) == EOF &&
				CalcCRC32(std::string_view((const char *)data.Data(), h.size)) == h.crc;
		}
		fclose(f);
		if (!ok)
		{
			data.Clear();
			corrupt = true;
			return false;
		}
		format = h.format;
		return true;
	}
}

namespace ProgramBinaryCache
{
	FString MakeKey(const char *ns, const char *driver, const char *pipeline, const FString &vertex, const FString &fragment)
	{
		return Md5Hex({ ns, driver, pipeline,
			std::string_view(vertex.GetChars(), vertex.Len()),
			std::string_view(fragment.GetChars(), fragment.Len()) });
	}

	bool Load(const char *ns, const char *driver, const FString &key, uint32_t &format, TArray<uint8_t> &data)
	{
		format = 0;
		data.Clear();
		if (!r_programcache || key.Len() != 32)
			return false;

		const std::string &dir = DriverDirectory(ns, driver);
		std::string id = dir + "/" + key.GetChars();

		auto it = LruIndex.find(id);
		if (it != LruIndex.end())
		{
			Lru.splice(Lru.begin(), Lru, it->second);
			format = it->second->format;
			data = it->second->data;
			Counters.ramHits++;
			return true;
		}

		bool corrupt;
		std::string path = EntryPath(dir, key, ".bin");
		if (ReadEntry(path, key, format, data, corrupt))
		{
			Counters.diskHits++;
			Counters.bytesRead += data.Size();
			LruInsert(id, format, data);
			return true;
		}
		if (corrupt)
		{
			Counters.corrupt++;
			remove(path.c_str());
		}
		Counters.misses++;
		return false;
	}

	void Store(const char *ns, const char *driver, const FString &key, uint32_t format, const TArray<uint8_t> &data)
	{
		if (!r_programcache || key.Len() != 32 || data.Size() == 0 || data.Size() > MaxBinarySize)
			return;

		const std::string &dir = DriverDirectory(ns, driver);
		std::string id = dir + "/" + key.GetChars();
		LruInsert(id, format, data);

		EntryHeader h;
		memcpy(h.magic, EntryMagic, 4);
		h.version = EntryVersion;
		h.format = format;
		h.size = data.Size();
		h.crc = CalcCRC32(std::string_view((const char *)data.Data(), data.Size()));
		memcpy(h.key, key.GetChars(), 32);

		std::string tmp = EntryPath(dir, key, ".tmp");
		std::string finalPath = EntryPath(dir, key, ".bin");
		FILE *f = fopen(tmp.c_str(), "wb");
		bool ok = f != nullptr;
		if (ok)
		{
			ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(data.Data(), 1, data.Size(), f) == data.Size();
			ok = (fclose(f) == 0) && ok;
		}
#ifdef _WIN32
		if (ok) remove(finalPath.c_str());	// rename() does not replace on Windows
#endif
		if (ok)
			ok = rename(tmp.c_str(), finalPath.c_str()) == 0;
		if (!ok)
		{
			remove(tmp.c_str());
			Counters.writeFailures++;
			return;
		}
		Counters.stores++;
		Counters.bytesWritten += sizeof(h) + data.Size();
	}

	void Reject(const char *ns, const char *driver, const FString &key)
	{
		if (key.Len() != 32)
			return;
		const std::string &dir = DriverDirectory(ns, driver);
		LruErase(dir + "/" + key.GetChars());
		remove(EntryPath(dir, key, ".bin").c_str());
		Counters.rejects++;
	}

	void TrimMemory(bool all)
	{
		size_t before = LruBytes;
		LruEnforce(all ? 0 : RamBudget() / 2);
		Counters.trimmedBytes += before - LruBytes;
	}

	void RequestTrim()
	{
		TrimPending = true;
	}

	bool TrimRequested()
	{
		return TrimPending;
	}

	void ProcessPendingTrim()
	{
		if (TrimPending.exchange(false))
			TrimMemory(true);
	}

	void PrintStats()
	{
		Printf("Program binary cache: %s, root %s\n", r_programcache ? "on" : "off", RootDirectory().c_str());
		Printf("  RAM: %u entries, %u KB (peak %u KB, budget %d KB); %u evictions, %llu KB evicted, %llu KB dropped on memory pressure\n",
			(unsigned)Lru.size(), (unsigned)(LruBytes / 1024), (unsigned)(Counters.peakRam / 1024), (int)r_programcache_ram_kb,
			Counters.evictions, (unsigned long long)(Counters.bytesEvicted / 1024), (unsigned long long)(Counters.trimmedBytes / 1024));
		Printf("  hits: %u RAM, %u disk; misses (compiled from source) %u; stored %u; rejected by driver %u; corrupt %u; write failures %u\n",
			Counters.ramHits, Counters.diskHits, Counters.misses, Counters.stores, Counters.rejects, Counters.corrupt, Counters.writeFailures);
		Printf("  disk traffic: %llu KB read, %llu KB written\n",
			(unsigned long long)(Counters.bytesRead / 1024), (unsigned long long)(Counters.bytesWritten / 1024));
	}
}

extern "C"
#if defined(__GNUC__) || defined(__clang__)
__attribute__((used)) __attribute__((visibility("default")))
#endif
void UZDoom_SetShaderCacheDirectory(const char *path)
{
	RootOverride = (path && *path) ? path : "";
	PreparedDirs.clear();
	Lru.clear();
	LruIndex.clear();
	LruBytes = 0;
}

CCMD(r_programcache_stats)
{
	ProgramBinaryCache::PrintStats();
}

CCMD(r_programcache_trim)
{
	ProgramBinaryCache::TrimMemory(true);
	ProgramBinaryCache::PrintStats();
}
