// 用真实的 fmodstudio.dll 和构建出的插件验证补丁，不需要启动游戏
// 用法：fmodcrashfix_test_patch <游戏 win64 目录> <FmodCrashFix.dll 路径>
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	// FMOD 2.3.14 (build 164239)
	constexpr std::uintptr_t RebindRva = 0xE2F60;
	constexpr std::uintptr_t CrashRva = 0x1058FE;
	constexpr std::uintptr_t PatchRva = 0xE2FBF;

	constexpr std::int32_t Key = 0x11111111;
	constexpr std::int32_t Id = 0x22222222;

	using Rebind_t = int (*)(void*, const std::int32_t*);

	struct Block
	{
		alignas(16) std::byte data[0x400]{};

		template <class T>
		T& at(std::size_t a_offset) noexcept
		{
			return *reinterpret_cast<T*>(data + a_offset);
		}

		operator void*() noexcept { return data; }
	};

	Block  g_found;
	Block  g_modelObject;

	void* Find(void*, const void*, void*)
	{
		return g_found.data;
	}

	void* g_modelVtable[2]{ nullptr, reinterpret_cast<void*>(&Find) };

	// 一组最小的假对象，字段含义见 README 的内存布局
	struct Scene
	{
		Block        container;
		Block        item;
		Block        owner;
		Block        model;
		Block        node;
		std::int32_t context[4]{ Id, 1, 2, 3 };
		void*        array[1]{};

		explicit Scene(bool a_hasModel)
		{
			array[0] = item.data;

			container.at<const void*>(0x10) = context;
			container.at<std::int32_t>(0x18) = 1;
			container.at<void**>(0x30) = array;
			container.at<std::int32_t>(0x38) = 1;

			item.at<void*>(0x20) = owner.data;
			item.at<std::int32_t>(0x28) = Key;
			item.at<std::int32_t>(0x38) = 0;
			item.at<std::int32_t>(0x48) = 2;

			if (a_hasModel) {
				model.at<void**>(0) = g_modelVtable;
				owner.at<void*>(0x8) = model.data;

				g_found.at<void*>(0) = g_modelObject.data;
				g_modelObject.at<std::int32_t>(0x2F0) = Id;

				void* const head = owner.data + 0x28;
				owner.at<void*>(0x28) = node.data;
				owner.at<void*>(0x30) = node.data;
				node.at<void*>(0) = head;
				node.at<void*>(8) = head;
				node.at<std::int32_t>(0x28) = Id;
				node.at<std::int32_t>(0x38) = 1;
				node.at<std::int32_t>(0x48) = 1;
			}
		}
	};

	struct CallResult
	{
		bool           faulted{ false };
		std::uintptr_t faultAddress{ 0 };
		int            value{ -1 };
	};

	CallResult SafeCall(Rebind_t a_rebind, void* a_container)
	{
		CallResult         result;
		const std::int32_t key = Key;
		__try {
			result.value = a_rebind(a_container, &key);
		} __except (
			result.faultAddress = reinterpret_cast<std::uintptr_t>(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
			GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
			result.faulted = true;
		}
		return result;
	}

	int g_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		std::printf("  [%s] %s\n", a_ok ? "PASS" : "FAIL", a_what);
		if (!a_ok) {
			++g_failures;
		}
	}

	void CheckMissingModelCrashes(std::uintptr_t a_base, Rebind_t a_rebind)
	{
		Scene      scene{ false };
		const auto result = SafeCall(a_rebind, scene.container);
		Check(result.faulted && result.faultAddress == a_base + CrashRva, "unpatched: missing model faults at the crash site");
	}

	void CheckMissingModelSkipped(Rebind_t a_rebind)
	{
		Scene      scene{ false };
		const auto result = SafeCall(a_rebind, scene.container);
		Check(!result.faulted && result.value == 0, "patched: missing model returns FMOD_OK");
		Check(scene.array[0] == scene.item.data, "patched: skipped entry stays in the array");
		Check(scene.item.at<std::int32_t>(0x48) == 2, "patched: skipped entry keeps its reference count");
		Check(scene.item.at<std::int32_t>(0x38) == 0, "patched: skipped entry keeps its state");
	}

	void CheckNormalPath(Rebind_t a_rebind, const char* a_label)
	{
		Scene      scene{ true };
		const auto result = SafeCall(a_rebind, scene.container);
		const auto name = std::string{ a_label };
		Check(!result.faulted && result.value == 0, (name + ": valid model returns FMOD_OK").c_str());
		Check(scene.array[0] == scene.node.data, (name + ": entry is rebound to the resolved node").c_str());
		Check(scene.item.at<std::int32_t>(0x48) == 1, (name + ": old entry is released").c_str());
		Check(scene.node.at<std::int32_t>(0x48) == 2, (name + ": new entry is referenced").c_str());
	}
}

int wmain(int a_argc, wchar_t* a_argv[])
{
	if (a_argc != 3) {
		std::puts("usage: fmodcrashfix_test_patch <game win64 dir> <FmodCrashFix.dll>");
		return 2;
	}

	SetDllDirectoryW(a_argv[1]);
	const auto fmod = LoadLibraryW((std::wstring{ a_argv[1] } + L"\\fmodstudio.dll").c_str());
	if (!fmod) {
		std::printf("failed to load fmodstudio.dll: %lu\n", GetLastError());
		return 2;
	}

	const auto base = reinterpret_cast<std::uintptr_t>(fmod);
	const auto rebind = reinterpret_cast<Rebind_t>(base + RebindRva);

	std::puts("before patch:");
	CheckMissingModelCrashes(base, rebind);
	CheckNormalPath(rebind, "unpatched");

	if (!LoadLibraryW(a_argv[2])) {
		std::printf("failed to load plugin: %lu\n", GetLastError());
		return 2;
	}

	std::puts("after patch:");
	Check(std::memcmp(reinterpret_cast<const void*>(base + PatchRva), "\xFF\x25\x00\x00\x00\x00", 6) == 0, "patch is written at the expected site");
	CheckMissingModelSkipped(rebind);
	CheckNormalPath(rebind, "patched");

	std::printf("%d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
