includes("../xmake/plugin.lua")

target("custom_type_effects")
    add_rules("dd.plugin")
    set_version("1.0.0")

    -- 预制功能：与 BuffIcon 一样装进 mods\DD Mod Lib，由加载器强制加载、始终启用
    set_values("dd.mod", "DD Mod Lib")

    set_pcxxheader("src/PCH.h")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")

    -- 配置模板与其它预制插件一样放到 DDModLoader\cfg，不覆盖玩家已改的数值。
    -- 是否部署由 DDPath 决定（DD.ModLoader/tools/env.sh 只在 DD_DEPLOY=1 时导出它）。
    after_build(function (target)
        local game = os.getenv("DDPath")
        if not game or game == "" then
            return
        end

        local cfgdir = path.join(game, "DDModLoader", "cfg")
        os.mkdir(cfgdir)
        local cfg = path.join(target:scriptdir(), "mod", "CustomTypeEffects.cfg")
        if os.isfile(cfg) and not os.isfile(path.join(cfgdir, "CustomTypeEffects.cfg")) then
            os.cp(cfg, cfgdir)
            cprint("${bright green}deployed${clear} CustomTypeEffects.cfg -> %s", cfgdir)
        end
    end)
target_end()
