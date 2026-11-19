includes("../xmake/plugin.lua")

target("SpineFix")
    add_rules("dd.plugin")
    set_version("1.0.0")
    set_values("dd.license", "GPL-3.0 License")

    -- 和 FmodCrashFix / DarkestCrashFix 一样是预制功能：装进 mods\DD Mod Lib，始终启用
    set_values("dd.mod", "DD Mod Lib")

    add_syslinks("opengl32")

    set_pcxxheader("src/PCH.h")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
target_end()

-- 脱离游戏验证时间线修补逻辑的小工具，默认不构建：xmake build spinefix_test
target("spinefix_test")
    set_kind("binary")
    set_default(false)
    set_group("SpineFix/tools")
    set_languages("c++23")
    set_runtimes(is_mode("debug") and "MTd" or "MT")
    set_exceptions("cxx")
    add_cxflags("/utf-8", { tools = "cl" })
    add_includedirs("src")
    add_files("src/Timeline.cpp", "src/Fix.cpp", "tools/test_timeline.cpp")
target_end()
