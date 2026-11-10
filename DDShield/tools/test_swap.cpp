#include "hooks.h"
#include "render.h"

// 用游戏自带的 SDL2.dll 验证 SDL_GL_SwapWindow 的长跳 Hook，不需要启动游戏
// 用法：ddshield_test_swap <SDL2.dll 路径>
int main(int argc, char** argv) {
    if (argc < 2) return 2;

    HMODULE sdl = LoadLibraryA(argv[1]);
    if (!sdl) {
        printf("failed to load %s\n", argv[1]);
        return 3;
    }

    const auto sdlInit = reinterpret_cast<int (*)(std::uint32_t)>(GetProcAddress(sdl, "SDL_Init"));
    const auto createWindow = reinterpret_cast<void* (*)(const char*, int, int, int, int, std::uint32_t)>(GetProcAddress(sdl, "SDL_CreateWindow"));
    const auto createContext = reinterpret_cast<void* (*)(void*)>(GetProcAddress(sdl, "SDL_GL_CreateContext"));
    const auto swapWindow = reinterpret_cast<void (*)(void*)>(GetProcAddress(sdl, "SDL_GL_SwapWindow"));
    const auto getError = reinterpret_cast<const char* (*)()>(GetProcAddress(sdl, "SDL_GetError"));
    const auto clearError = reinterpret_cast<void (*)()>(GetProcAddress(sdl, "SDL_ClearError"));

    constexpr std::uint32_t InitVideo = 0x20;
    constexpr std::uint32_t WindowOpenGL = 0x2;
    constexpr std::uint32_t WindowHidden = 0x8;

    if (sdlInit(InitVideo) != 0) {
        printf("SDL_Init failed: %s\n", getError());
        return 4;
    }

    void* window = createWindow("ddshield_test_swap", 0, 0, 64, 64, WindowOpenGL | WindowHidden);
    if (!window || !createContext(window)) {
        printf("window or GL context failed: %s\n", getError());
        return 5;
    }

    if (!dds::HookSwapWindow()) {
        return 6;
    }

    // 窗口指针必须原样传给真实函数，传错时 SDL 会报 Invalid window
    clearError();
    swapWindow(window);
    const std::string validError = getError();

    // 空指针只有真实函数会拒绝，用来确认 epilog 确实跳到了真实函数
    clearError();
    swapWindow(nullptr);
    const std::string nullError = getError();

    const auto hooked = *reinterpret_cast<const std::uint8_t*>(swapWindow) == 0xE9;
    printf("thunk patched: %d\noverlay ready (hook ran): %d\nvalid window error: '%s'\nnull window error: '%s'\n",
        hooked, dds::render::Ready(), validError.c_str(), nullError.c_str());

    return hooked && dds::render::Ready() && validError.empty() && !nullError.empty() ? 0 : 1;
}
