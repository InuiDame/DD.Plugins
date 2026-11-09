set_project("DD.Plugins")

includes("xmake/plugin.lua")

for _, file in ipairs(os.files(path.join(os.scriptdir(), "*", "xmake.lua"))) do
    local name = path.filename(path.directory(file))
    if name ~= "PluginTemplate" and name ~= "extern" and name ~= "xmake" then
        includes(file)
    end
end
