includes("../xmake/plugin.lua")

target("hero_level_cap")
    add_rules("dd.plugin")
    set_version("1.0.0")
    -- 作为独立 DLL MOD 部署到 mods\HeroLevelCap，由加载器 GUI 勾选启用
    set_values("dd.mod", "HeroLevelCap")

    set_pcxxheader("src/PCH.h")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")

    -- 只补缺失的 MOD 元数据，不覆盖玩家改过的 project.xml；同步 7 级以上称号本地化
    after_build(function (target)
        local game = os.getenv("DDPath")
        if not game or game == "" then return end
        local dir = path.join(game, "mods", target:values("dd.mod"))
        local src = path.join(os.scriptdir(), "mod", "project.xml")
        local dst = path.join(dir, "project.xml")
        if os.isfile(src) and not os.isfile(dst) then
            os.cp(src, dst)
            cprint("${bright green}deployed${clear} project.xml -> %s", dir)
        end
        -- ModDataPath 必须是实际部署目录，否则 campaign/ 与 localization/ 不会被加载
        if os.isfile(dst) then
            local content = io.readfile(dst)
            if content then
                local fixed = content:gsub("<ModDataPath>.-</ModDataPath>",
                    "<ModDataPath>" .. dir:gsub("\\", "/") .. "/</ModDataPath>")
                if fixed ~= content then
                    io.writefile(dst, fixed)
                end
            end
        end
        -- 本地化：str_resolve_7..26，让 7 级以上不再显示 [<str_resolve_7>]
        local locdir = path.join(os.scriptdir(), "mod", "localization")
        if os.isdir(locdir) then
            os.mkdir(path.join(dir, "localization"))
            for _, f in ipairs(os.files(path.join(locdir, "*.xml"))) do
                os.cp(f, path.join(dir, "localization"))
            end
            cprint("${bright green}deployed${clear} localization -> %s", dir)
        end
        -- 数据覆盖：更长的 resolve_level_thresholds，让游戏自己解析并分配到 8 项。
        -- 这是唯一安全的扩容方式——不碰游戏内存，靠游戏自己的加载器建表。
        local data = path.join(os.scriptdir(), "mod", "campaign", "roster", "roster.variables.json")
        if os.isfile(data) then
            os.mkdir(path.join(dir, "campaign", "roster"))
            os.cp(data, path.join(dir, "campaign", "roster"))
            cprint("${bright green}deployed${clear} roster.variables.json -> %s", dir)
        end
    end)
target_end()

-- 离线单测（默认不构建）：xmake build hero_level_cap_test && build/windows/x64/releasedbg/hero_level_cap_test.exe
target("hero_level_cap_test")
    set_kind("binary")
    set_default(false)
    set_group("HeroLevelCap/tools")
    set_languages("c++23")
    set_runtimes(is_mode("debug") and "MTd" or "MT")
    set_exceptions("cxx")
    set_warnings("all")

    add_includedirs("src")
    set_pcxxheader("tools/TestPCH.h")

    add_files("tools/test_rules.cpp", "src/Rules.cpp")
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
