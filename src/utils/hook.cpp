#include <std_include.hpp>
#include "hook.hpp"

namespace utils::hook
{
	namespace
	{
		std::once_flag mh_once;

		void ensure_minhook()
		{
			std::call_once(mh_once, []()
			{
				MH_Initialize();
			});
		}

		void write_protected(void* place, const void* data, const size_t length)
		{
			DWORD old_protect{};
			VirtualProtect(place, length, PAGE_EXECUTE_READWRITE, &old_protect);
			std::memcpy(place, data, length);
			VirtualProtect(place, length, old_protect, &old_protect);
			FlushInstructionCache(GetCurrentProcess(), place, length);
		}

		// ---------------------------------------------------------------- near trampolines

		constexpr size_t STUB_SIZE = 16; // FF 25 00 00 00 00 <abs64> + 2 pad
		constexpr size_t PAGE_SIZE_ALLOC = 0x10000;

		struct trampoline_page
		{
			std::uint8_t* base{};
			size_t used{};
		};

		std::mutex tramp_mutex;
		std::vector<trampoline_page> tramp_pages;
		std::map<std::pair<const void*, const void*>, void*> tramp_cache; // (page-base, target) -> stub

		bool in_rel32_range(const void* from, const void* to)
		{
			const auto diff = reinterpret_cast<std::intptr_t>(to) - reinterpret_cast<std::intptr_t>(from);
			return diff > static_cast<std::intptr_t>(INT32_MIN) + 0x10000 && diff < static_cast<std::intptr_t>(INT32_MAX) - 0x10000;
		}

		std::uint8_t* allocate_near(const void* address)
		{
			SYSTEM_INFO si{};
			GetSystemInfo(&si);
			const auto granularity = static_cast<std::uintptr_t>(si.dwAllocationGranularity);

			const auto origin = reinterpret_cast<std::uintptr_t>(address);
			const std::uintptr_t range = 0x7FF00000ull;
			const auto min_addr = std::max<std::uintptr_t>(origin > range ? origin - range : 0, reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress));
			const auto max_addr = std::min<std::uintptr_t>(origin + range, reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress));

			// search upwards first, then downwards
			for (int dir = 0; dir < 2; ++dir)
			{
				auto cur = (origin / granularity) * granularity;
				while (dir == 0 ? cur < max_addr : cur > min_addr)
				{
					MEMORY_BASIC_INFORMATION mbi{};
					if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi)))
					{
						break;
					}

					if (mbi.State == MEM_FREE)
					{
						const auto aligned = ((reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + granularity - 1) / granularity) * granularity;
						if (aligned + PAGE_SIZE_ALLOC <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)
						{
							if (auto* mem = VirtualAlloc(reinterpret_cast<void*>(aligned), PAGE_SIZE_ALLOC, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
							{
								return static_cast<std::uint8_t*>(mem);
							}
						}
					}

					if (dir == 0)
					{
						cur = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
						cur = ((cur + granularity - 1) / granularity) * granularity;
					}
					else
					{
						if (reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) <= granularity) break;
						cur = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) - granularity;
					}
				}
			}

			return nullptr;
		}
	}

	bool is_readable(const void* address, const size_t size)
	{
		auto cur = reinterpret_cast<std::uintptr_t>(address);
		const auto end = cur + size;

		while (cur < end)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi)))
			{
				return false;
			}

			if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0)
			{
				return false;
			}

			cur = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		}

		return true;
	}

	void* get_near_trampoline(const void* near_address, const void* target)
	{
		std::lock_guard _(tramp_mutex);

		// reuse an existing page within range
		trampoline_page* page = nullptr;
		for (auto& p : tramp_pages)
		{
			if (in_rel32_range(near_address, p.base) && in_rel32_range(near_address, p.base + PAGE_SIZE_ALLOC))
			{
				if (const auto it = tramp_cache.find({p.base, target}); it != tramp_cache.end())
				{
					return it->second;
				}

				if (p.used + STUB_SIZE <= PAGE_SIZE_ALLOC)
				{
					page = &p;
					break;
				}
			}
		}

		if (!page)
		{
			auto* mem = allocate_near(near_address);
			if (!mem)
			{
				throw std::runtime_error("unable to allocate a near trampoline page");
			}

			tramp_pages.push_back({mem, 0});
			page = &tramp_pages.back();
		}

		auto* stub = page->base + page->used;
		page->used += STUB_SIZE;

		std::uint8_t code[STUB_SIZE]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
		const auto abs = reinterpret_cast<std::uint64_t>(target);
		std::memcpy(code + 6, &abs, sizeof(abs));
		code[14] = 0xCC;
		code[15] = 0xCC;
		std::memcpy(stub, code, STUB_SIZE);
		FlushInstructionCache(GetCurrentProcess(), stub, STUB_SIZE);

		tramp_cache[{page->base, target}] = stub;
		return stub;
	}

	// ---------------------------------------------------------------- detour

	detour::detour(void* place, void* target)
	{
		this->create(place, target);
	}

	detour::detour(const size_t place, void* target) : detour(reinterpret_cast<void*>(place), target)
	{
	}

	detour::~detour()
	{
		this->clear();
	}

	void detour::enable() const
	{
		MH_EnableHook(this->place_);
	}

	void detour::disable() const
	{
		MH_DisableHook(this->place_);
	}

	void detour::create(void* place, void* target)
	{
		ensure_minhook();
		this->clear();
		this->place_ = place;

		if (MH_CreateHook(this->place_, target, &this->original_) != MH_OK)
		{
			throw std::runtime_error(std::format("unable to create hook at location: {}", this->place_));
		}

		this->enable();
	}

	void detour::create(const size_t place, void* target)
	{
		this->create(reinterpret_cast<void*>(place), target);
	}

	void detour::clear()
	{
		if (this->place_)
		{
			MH_RemoveHook(this->place_);
		}

		this->place_ = nullptr;
		this->original_ = nullptr;
	}

	void* detour::get_original() const
	{
		return this->original_;
	}

	// ---------------------------------------------------------------- raw patches

	void nop(void* place, const size_t length)
	{
		DWORD old_protect{};
		VirtualProtect(place, length, PAGE_EXECUTE_READWRITE, &old_protect);
		std::memset(place, 0x90, length);
		VirtualProtect(place, length, old_protect, &old_protect);
		FlushInstructionCache(GetCurrentProcess(), place, length);
	}

	void nop(const size_t place, const size_t length)
	{
		nop(reinterpret_cast<void*>(place), length);
	}

	void copy(void* place, const void* data, const size_t length)
	{
		write_protected(place, data, length);
	}

	void copy(const size_t place, const void* data, const size_t length)
	{
		copy(reinterpret_cast<void*>(place), data, length);
	}

	bool is_relatively_far(const void* pointer, const void* data, const int offset)
	{
		const int64_t diff = reinterpret_cast<int64_t>(data) - (reinterpret_cast<int64_t>(pointer) + offset);
		const auto small_diff = static_cast<int32_t>(diff);
		return diff != static_cast<int64_t>(small_diff);
	}

	namespace
	{
		void write_branch(void* pointer, void* data, const std::uint8_t opcode)
		{
			if (is_relatively_far(pointer, data, 5))
			{
				data = get_near_trampoline(pointer, data);
			}

			std::uint8_t code[5]{opcode};
			const auto rel = static_cast<int32_t>(reinterpret_cast<std::intptr_t>(data) - (reinterpret_cast<std::intptr_t>(pointer) + 5));
			std::memcpy(code + 1, &rel, sizeof(rel));
			write_protected(pointer, code, sizeof(code));
		}
	}

	void call(void* pointer, void* data)
	{
		write_branch(pointer, data, 0xE8);
	}

	void call(const size_t pointer, void* data)
	{
		call(reinterpret_cast<void*>(pointer), data);
	}

	void call(const size_t pointer, const size_t data)
	{
		call(pointer, reinterpret_cast<void*>(data));
	}

	void jump(void* pointer, void* data)
	{
		write_branch(pointer, data, 0xE9);
	}

	void jump(const size_t pointer, void* data)
	{
		jump(reinterpret_cast<void*>(pointer), data);
	}

	void jump(const size_t pointer, const size_t data)
	{
		jump(pointer, reinterpret_cast<void*>(data));
	}

	void* get_call_target(const void* pointer)
	{
		const auto* p = static_cast<const std::uint8_t*>(pointer);
		const auto rel = *reinterpret_cast<const int32_t*>(p + 1);
		return const_cast<std::uint8_t*>(p + 5 + rel);
	}
}
