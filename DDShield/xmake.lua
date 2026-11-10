includes("../xmake/plugin.lua")

-- dll 之外的 mod 文件，dst 是相对 mod 文件夹的路径
local modfiles = {
    { src = path.join(os.scriptdir(), "DDShield.ini"), dst = "DDShield.ini" },
    { src = path.join(os.scriptdir(), "DDShield.effects.json"), dst = "DDShield.effects.json" },
}
local datadir = path.join(os.scriptdir(), "data", "mods", "DDShield")
for _, file in ipairs(os.files(path.join(datadir, "**"))) do
    table.insert(modfiles, { src = file, dst = path.relative(file, datadir) })
end

target("DDShield")
    add_rules("dd.plugin")
    set_version("1.3.0")
    set_values("dd.mod", "DDShield")
    set_values("dd.license", "GPL-3.0 License")

    set_pcxxheader("src/PCH.h")
    add_files("src/*.cpp")
    add_headerfiles("src/*.h")
    add_defines("_CRT_SECURE_NO_WARNINGS")
    add_syslinks("gdi32", "advapi32")

    -- dll 由 dd.plugin 部署，这里只补缺失的文件：DDShield.ini 可能被玩家调整过，
    -- 已发布 mod 的 project.xml 带 PublishedFileId，都不能覆盖
    after_build(function (target)
        local game = os.getenv("DDPath")
        if not game or game == "" then
            return
        end

        local dir = path.join(game, "mods", target:values("dd.mod"))
        for _, file in ipairs(modfiles) do
            local dst = path.join(dir, file.dst)
            if not os.isfile(dst) then
                os.cp(file.src, dst)
                cprint("${bright green}deployed${clear} %s -> %s", file.dst, dir)
            end
        end
    end)

    -- xmake install -o <目录> DDShield：生成完整的 mod 文件夹 <目录>\DDShield
    on_install(function (target)
        local dir = path.join(target:installdir(), target:values("dd.mod"))
        os.mkdir(dir)
        os.cp(target:targetfile(), dir)
        os.cp(path.join(target:scriptdir(), "README.md"), dir)
        for _, file in ipairs(modfiles) do
            os.cp(file.src, path.join(dir, file.dst))
        end
    end)
target_end()

-- 脱离游戏验证的小工具，默认不构建：xmake build ddshield_test_config
for name, sources in pairs({
    test_config  = { "config" },
    test_loc     = { "loc" },
    test_classes = { "shield", "config", "game" },
    test_swap    = { "hooks", "shield", "config", "game", "render", "loc" },
    test_shield  = { "hooks", "shield", "config", "game", "render", "loc" },
}) do
    target("ddshield_" .. name)
        add_rules("dd.plugin")
        set_version("1.3.0")
        set_values("dd.kind", "binary")
        set_default(false)
        set_group("DDShield/tools")

        set_pcxxheader("src/PCH.h")
        add_includedirs("src")
        add_defines("_CRT_SECURE_NO_WARNINGS")
        add_syslinks("gdi32", "advapi32")
        add_files("tools/" .. name .. ".cpp")
        for _, source in ipairs(sources) do
            add_files("src/" .. source .. ".cpp")
        end
    target_end()
end
