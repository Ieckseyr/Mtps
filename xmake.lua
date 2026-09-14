add_rules("mode.debug", "mode.release")

local function find_local_repo()
    local candidates = {
        path.join(os.projectdir(), ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", "MeowMenu", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", "MeowPAPI", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
        path.join(os.projectdir(), "..", "MeowSidebar", ".xmake", os.host(), os.arch(), "repositories", "liteldev-repo"),
    }
    for _, p in ipairs(candidates) do
        if os.exists(path.join(p, "packages", "l", "levilamina", "versions", "26_40_0.lua")) then return p end
    end
    return nil
end
local local_repo = find_local_repo()
if local_repo then
    add_repositories("liteldev-repo " .. local_repo)
else
    add_repositories("liteldev-repo https://github.com/LiteLDev/xmake-repo.git")
end

if is_config("target_type", "server") then
    add_requires("levilamina 26.40.0", {configs = {target_type = "server"}})
else
    add_requires("levilamina 26.40.0", {configs = {target_type = "client"}})
end

add_requires("levibuildscript")
add_requires("zlib")
add_requires("zstd")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

target("Mtps")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    add_cxflags("/EHa", "/utf-8", "/W4", "/Zm2000", "/wd4100", "/wd4244", "/wd4267", "/wd4189", "/wd4996", {force = true})
    add_defines("NOMINMAX", "UNICODE", "_AMD64_", "LL_MEMORY_OPERATORS")
    add_packages("levilamina", "zlib", "zstd")
    set_kind("shared")
    set_languages("c++23")
    set_symbols("debug")

    add_includedirs("../HologramLib/include")
    add_linkdirs("../HologramLib/build/windows/x64/release")
    add_links("HologramLib")

    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")

    if is_config("target_type", "server") then
        add_defines("LL_PLAT_S")
    else
        add_defines("LL_PLAT_C")
    end

    after_build(function (target)
        local output_dir = "$(projectdir)/bin/Mtps/"
        os.mkdir(output_dir)
        os.cp(target:targetfile(), output_dir)
        local pdb_file = path.join(path.directory(target:targetfile()), "Mtps.pdb")
        if os.exists(pdb_file) then
            os.cp(pdb_file, output_dir)
        end
        os.cp(target:targetfile(), "$(projectdir)/")
        if os.exists(pdb_file) then
            os.cp(pdb_file, "$(projectdir)/")
        end
    end)
