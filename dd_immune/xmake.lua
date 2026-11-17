includes("../xmake/plugin.lua")

target("dd_immune")
    add_rules("dd.plugin")
    set_version("1.0.0")

    -- 预制功能：与 CustomTypeEffects / BuffIcon 一样装进 mods\DD Mod Lib，
    -- 由加载器强制加载、始终启用，不按存档勾选。
    set_values("dd.mod", "DD Mod Lib")

    set_pcxxheader("src/PCH.h")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
target_end()

-- 离线单测（set_default(false)，CI 不会自动构建，需本地手动 build + run）
target("dd_immune_test")
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    set_runtimes(is_mode("debug") and "MTd" or "MT")
    set_exceptions("cxx")
    set_warnings("all")

    add_packages("nlohmann_json")
    add_includedirs("src")
    set_pcxxheader("tools/TestPCH.h")

    add_files("tools/test_immune.cpp", "src/Rules.cpp")
    add_headerfiles("src/Rules.h", "tools/TestPCH.h")

    add_cxflags(
        "/utf-8", "/permissive-", "/Zc:__cplusplus", "/Zc:preprocessor",
        "/Zc:alignedNew", "/Zc:auto", "/Zc:externConstexpr", "/Zc:forScope",
        "/Zc:hiddenFriend", "/Zc:implicitNoexcept", "/Zc:lambda",
        "/Zc:noexceptTypes", "/Zc:referenceBinding", "/Zc:rvalueCast",
        "/Zc:sizedDealloc", "/Zc:strictStrings", "/Zc:ternary",
        "/Zc:threadSafeInit", "/Zc:wchar_t", "/wd4200",
        { tools = "cl" })
target_end()
