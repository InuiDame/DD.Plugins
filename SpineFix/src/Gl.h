#pragma once

// 只声明用到的几个 GL 入口，避免引入 GL 头文件（引擎自己带了一份 glad 风格的加载器）。
// x64 忽略 __stdcall，和 opengl32.dll 的导出一致。

extern "C"
{
	__declspec(dllimport) void __stdcall glEnable(unsigned int a_cap);
	__declspec(dllimport) void __stdcall glDisable(unsigned int a_cap);
	__declspec(dllimport) unsigned char __stdcall glIsEnabled(unsigned int a_cap);
	__declspec(dllimport) void __stdcall glGetIntegerv(unsigned int a_pname, int* a_params);
	__declspec(dllimport) void __stdcall glBlendFunc(unsigned int a_sfactor, unsigned int a_dfactor);
}

namespace SpineFix::gl
{
	constexpr unsigned int Blend              = 0x0BE2;
	constexpr unsigned int One                = 1;
	constexpr unsigned int DstColor           = 0x0306;
	constexpr unsigned int OneMinusSrcColor   = 0x0301;
	constexpr unsigned int OneMinusSrcAlpha   = 0x0303;
	constexpr unsigned int FuncAdd            = 0x8006;
	constexpr unsigned int BlendSrcRgb        = 0x80C9;
	constexpr unsigned int BlendDstRgb        = 0x80C8;
	constexpr unsigned int BlendSrcAlpha      = 0x80CB;
	constexpr unsigned int BlendDstAlpha      = 0x80CA;
	constexpr unsigned int BlendEquationRgb   = 0x8009;
	constexpr unsigned int BlendEquationAlpha = 0x883D;

	using BlendFuncSeparate_t     = void (*)(unsigned int, unsigned int, unsigned int, unsigned int);
	using BlendEquationSeparate_t = void (*)(unsigned int, unsigned int);

	// 引擎自己用 glad 风格加载器取址，表在这两个 RVA 上（knowledge/dd1/spine.md「渲染」：
	// [117CD90] = glBlendFuncSeparate，[117CFA0] = glBlendEquationSeparate）。
	// 不走 wglGetProcAddress：驱动对不支持的扩展会返回非空但不可调用的桩，调用即崩。
	constexpr std::uintptr_t BlendFuncSeparateSlot     = 0x117CD90;
	constexpr std::uintptr_t BlendEquationSeparateSlot = 0x117CFA0;

	// 指针小于这个值一定是桩或错误码，不能调用
	constexpr std::uintptr_t MinPlausiblePointer = 0x10000;

	// 还要确认落在一个已加载模块的可执行页里，RVA 对不上时宁可退回 glBlendFunc 也不乱调
	inline bool IsCallable(void* a_pointer) noexcept
	{
		if (reinterpret_cast<std::uintptr_t>(a_pointer) < MinPlausiblePointer) {
			return false;
		}

		MEMORY_BASIC_INFORMATION info{};
		if (::VirtualQuery(a_pointer, &info, sizeof(info)) == 0 || info.Type != MEM_IMAGE) {
			return false;
		}

		constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		return (info.Protect & executable) != 0;
	}

	inline void* FromGameTable(std::uintptr_t a_rva) noexcept
	{
		const auto base = dku::Hook::Module::get().base();
		auto* pointer = reinterpret_cast<void*>(*reinterpret_cast<std::uintptr_t*>(base + a_rva));
		return IsCallable(pointer) ? pointer : nullptr;
	}

	inline BlendFuncSeparate_t ResolveBlendFuncSeparate() noexcept
	{
		// 取不到时留空下次再试，不把 nullptr 缓存住
		static BlendFuncSeparate_t cached{ nullptr };
		if (!cached) {
			cached = reinterpret_cast<BlendFuncSeparate_t>(FromGameTable(BlendFuncSeparateSlot));
		}
		return cached;
	}

	inline BlendEquationSeparate_t ResolveBlendEquationSeparate() noexcept
	{
		static BlendEquationSeparate_t cached{ nullptr };
		if (!cached) {
			cached = reinterpret_cast<BlendEquationSeparate_t>(FromGameTable(BlendEquationSeparateSlot));
		}
		return cached;
	}

	struct State
	{
		int  srcRgb{ One };
		int  dstRgb{ OneMinusSrcAlpha };
		int  srcAlpha{ One };
		int  dstAlpha{ OneMinusSrcAlpha };
		int  equationRgb{ FuncAdd };
		int  equationAlpha{ FuncAdd };
		bool enabled{ true };
	};

	inline State Save()
	{
		State state;
		glGetIntegerv(BlendSrcRgb, &state.srcRgb);
		glGetIntegerv(BlendDstRgb, &state.dstRgb);
		glGetIntegerv(BlendSrcAlpha, &state.srcAlpha);
		glGetIntegerv(BlendDstAlpha, &state.dstAlpha);
		glGetIntegerv(BlendEquationRgb, &state.equationRgb);
		glGetIntegerv(BlendEquationAlpha, &state.equationAlpha);
		state.enabled = glIsEnabled(Blend) != 0;
		return state;
	}

	// 贴图是预乘的，所以正常混合是 ONE / ONE_MINUS_SRC_ALPHA，加法是 ONE / ONE。
	// 拿不到 glBlendFuncSeparate 时退回 glBlendFunc：RGB 结果一样，只是 alpha 通道一起变。
	inline void SetFuncSeparate(BlendFuncSeparate_t a_api, unsigned int a_src, unsigned int a_dst) noexcept
	{
		if (a_api) {
			a_api(a_src, a_dst, One, OneMinusSrcAlpha);
		} else {
			glBlendFunc(a_src, a_dst);
		}
	}

	inline void SetEquation(BlendEquationSeparate_t a_api, unsigned int a_rgb, unsigned int a_alpha) noexcept
	{
		if (a_api) {
			a_api(a_rgb, a_alpha);
		}
	}

	inline void SetEnabled(bool a_enabled) noexcept
	{
		if (a_enabled) {
			glEnable(Blend);
		} else {
			glDisable(Blend);
		}
	}
}
