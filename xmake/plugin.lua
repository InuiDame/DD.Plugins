set_xmakever("2.8.5")
set_allowedplats("windows")
set_allowedarchs("x64")
set_defaultarchs("windows|x64")
set_defaultmode("releasedbg")

add_requires("nlohmann_json", "spdlog", "xbyak", "simpleini")

rule("dd.plugin")
    add_deps("mode.debug", "mode.releasedbg", "mode.release")

    on_load(function (target)
        local root = path.directory(os.scriptdir())

        local dkutil
        for _, dir in ipairs({
            path.join(target:scriptdir(), "extern", "DKUtil"),
            path.join(root, "extern", "DKUtil"),
            os.getenv("DKUtilPath") or "",
        }) do
            if dir ~= "" and os.isfile(path.join(dir, "include", "DKUtil", "Hook.hpp")) then
                dkutil = path.absolute(dir)
                break
            end
        end
        if not dkutil then
            raise("%s: DKUtil not found, set DKUtilPath or run \"!update.ps1 -Extern\"", target:name())
        end
        target:data_set("dd.dkutil", dkutil)

        target:set("kind", target:values("dd.kind") or "shared")
        target:set("languages", "c++23")
        target:set("runtimes", is_mode("debug") and "MTd" or "MT")
        target:set("exceptions", "cxx")
        target:set("warnings", "all")

        target:add("includedirs", path.join(dkutil, "include"))
        target:add("packages", "nlohmann_json", "spdlog", "xbyak", "simpleini")
        target:add("syslinks", "ole32", "shell32", "user32", "version")
        target:add("defines", "UNICODE", "_UNICODE")
        target:add("cxflags",
            "/utf-8",
            "/permissive-",
            "/Zc:__cplusplus",
            "/Zc:preprocessor",
            "/Zc:alignedNew",
            "/Zc:auto",
            "/Zc:externConstexpr",
            "/Zc:forScope",
            "/Zc:hiddenFriend",
            "/Zc:implicitNoexcept",
            "/Zc:lambda",
            "/Zc:noexceptTypes",
            "/Zc:referenceBinding",
            "/Zc:rvalueCast",
            "/Zc:sizedDealloc",
            "/Zc:strictStrings",
            "/Zc:ternary",
            "/Zc:threadSafeInit",
            "/Zc:wchar_t",
            "/wd4200",
            { tools = "cl" })

        local configdir = path.join(target:autogendir(), "config")
        target:set("configdir", configdir)
        target:add("configfiles", path.join(root, "xmake", "Plugin.h.in"), { filename = "Plugin.h" })
        target:set("configvar", "PLUGIN_NAME", target:name())
        target:set("configvar", "PLUGIN_AUTHOR", os.getenv("DDPluginAuthor") or "")
        target:set("configvar", "PLUGIN_LICENSE", target:values("dd.license") or "MIT License")
        target:add("includedirs", configdir)
        target:add("files", path.join(root, "xmake", "version.rc"))
    end)

    after_build(function (target)
        local game = os.getenv("DDPath")
        local mod = target:values("dd.mod")
        if not game or game == "" or not mod then
            return
        end

        local dir = path.join(game, "mods", mod)
        os.mkdir(dir)
        os.cp(target:targetfile(), dir)
        cprint("${bright green}deployed${clear} %s -> %s", path.filename(target:targetfile()), dir)
    end)
rule_end()
