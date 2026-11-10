includes("../xmake/plugin.lua")

target("FmodCrashFix")
    add_rules("dd.plugin")
    set_version("1.0.0")
    set_values("dd.mod", "DD Mod Lib")

    set_pcxxheader("src/PCH.h")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
target_end()

-- 脱离游戏验证补丁的小工具，默认不构建：xmake build fmodcrashfix_test_patch
target("fmodcrashfix_test_patch")
    set_kind("binary")
    set_default(false)
    set_group("FmodCrashFix/tools")
    set_languages("c++23")
    set_runtimes(is_mode("debug") and "MTd" or "MT")
    add_cxflags("/utf-8", { tools = "cl" })
    add_files("tools/test_patch.cpp")
target_end()
