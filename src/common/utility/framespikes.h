#pragma once

// Frame spike counters: work that is done the first time something is needed and can stall a frame
// (a texture decoded and uploaded on first sight, a shader program built, a sound decoded on first
// play), plus the swap. The renderer logs one line for a frame much longer than the ones before it,
// with what that frame spent its time on (gl_frame_spikes).

#include <atomic>
#include <chrono>
#include <cstdint>

namespace FrameSpikes
{
	enum Counter
	{
		TextureCreates,
		ProgramBuilds,
		SoundLoads,
		Swap,
		CounterCount
	};

	struct CounterState
	{
		std::atomic<int64_t> calls{0};
		std::atomic<int64_t> us{0};
		std::atomic<int64_t> slowestUs{0};
	};

	inline CounterState Counters[CounterCount];

	inline int64_t NowUs()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	inline void Record(Counter counter, int64_t us)
	{
		CounterState &state = Counters[counter];
		state.calls.fetch_add(1, std::memory_order_relaxed);
		state.us.fetch_add(us, std::memory_order_relaxed);
		int64_t slowest = state.slowestUs.load(std::memory_order_relaxed);
		while (us > slowest && !state.slowestUs.compare_exchange_weak(slowest, us, std::memory_order_relaxed)) {}
	}

	// Times its own lifetime into a counter (when active)
	class Scope
	{
		Counter mCounter;
		int64_t mStart;

	public:
		explicit Scope(Counter counter, bool active = true) : mCounter(counter), mStart(active ? NowUs() : -1) {}
		~Scope() { if (mStart >= 0) Record(mCounter, NowUs() - mStart); }

		Scope(const Scope &) = delete;
		Scope &operator=(const Scope &) = delete;
	};
}
