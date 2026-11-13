includes("../xmake/plugin.lua")

target("scroll_wheel")
    add_rules("dd.plugin")
    set_version("1.0.0")

    -- S2：不再 set_values("dd.mod")。根 xmake/plugin.lua 的 after_build 只判断
    -- DDPath + dd.mod 就无条件 os.cp DLL 进真实安装（p4-review S2 证实），
    -- 而本环境 DDPath 实测已指向真实安装。改由本 target 自己受控部署：
    -- 必须同时 DD_DEPLOY=1 且 DDPath 非空，构建默认绝不触碰真实游戏目录。
    set_pcxxheader("src/PCH.h")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    -- v9：自绘滚动条已移除，不再链接 gdi32，DLL 不创建任何窗口。

    -- 部署（需显式允许）：DD_DEPLOY=1 + DDPath 非空时，DLL 复制进 mods\DD Mod Lib
    -- （由加载器强制加载），cfg 模板仅在目标不存在时复制，绝不覆盖玩家配置。
    after_build(function (target)
        if os.getenv("DD_DEPLOY") ~= "1" then
            return
        end
        local game = os.getenv("DDPath")
        if not game or game == "" then
            return
        end

        local moddir = path.join(game, "mods", "DD Mod Lib")
        os.mkdir(moddir)
        os.cp(target:targetfile(), moddir)
        cprint("${bright green}deployed${clear} %s -> %s", path.filename(target:targetfile()), moddir)

        local cfgdir = path.join(game, "DDModLoader", "cfg")
        os.mkdir(cfgdir)
        local src = path.join(target:scriptdir(), "mod", "ScrollWheel.cfg")
        local dst = path.join(cfgdir, "ScrollWheel.cfg")
        if os.isfile(src) and not os.isfile(dst) then
            os.cp(src, dst)
            cprint("${bright green}deployed${clear} ScrollWheel.cfg -> %s", cfgdir)
        end
    end)
target_end()

-- 脱离游戏验证纯逻辑 / 特征码 / 全有或全无校验的小工具，默认不构建。
-- 用法：xmake build scroll_wheel_test
--       build\windows\x64\releasedbg\scroll_wheel_test.exe <Darkest.exe 路径>
target("scroll_wheel_test")
    set_kind("binary")
    set_default(false)
    set_group("ScrollWheel/tools")
    set_languages("c++23")
    set_runtimes(is_mode("debug") and "MTd" or "MT")
    set_exceptions("cxx")
    add_defines("NOMINMAX", "_CRT_SECURE_NO_WARNINGS")
    add_cxflags("/utf-8", { tools = "cl" })
    add_includedirs("src")
    add_packages("xbyak") -- 单测生成 cave 机器码并做字节断言 / 外部 capstone 反汇编
    -- 直接编译生产源码，复用同一套 cfg / 状态机 / 校验逻辑
    add_files("src/Wheel.cpp", "src/Config.cpp", "src/Game.cpp", "tools/test_scroll.cpp")
target_end()
