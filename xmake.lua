add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

add_requires("levilamina 26.60.0", {configs = {target_type = "client"}})
add_requires("levibuildscript")
add_requires("nlohmann_json v3.12.0", "stb 2025.03.14", "zlib v1.3.2")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

-- LeviLamina 26.60 headers need the clang frontend (LLVM 22, clang-cl).
set_toolchains("clang-cl")

-- Resource packs are NOT bundled: LeviLamina would auto-mount mods/<name>/resource_packs/*,
-- forcing the effect on. Packs (packs/TesseraDemo, converted ones under local/) are
-- installed as normal resource packs so players switch them in Global Resources.

target("Tessera")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {modVersion = "1.0.0"})
    add_cxflags("/utf-8", "/W4", "/EHs")
    add_cxflags(
        "-Wno-microsoft-cast",
        "-Wno-invalid-offsetof",
        "-Wno-c++2b-extensions",
        "-Wno-microsoft-include",
        "-Wno-overloaded-virtual",
        "-Wno-ignored-qualifiers",
        "-Wno-missing-field-initializers",
        "-Wno-potentially-evaluated-expression",
        "-Wno-pragma-system-header-outside-header",
        {tools = {"clang_cl"}}
    )
    add_defines("NOMINMAX", "UNICODE")
    add_packages("levilamina", "nlohmann_json", "stb", "zlib")
    add_syslinks("user32") -- hot-reload key (GetAsyncKeyState)
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_files("src/**.cpp")
    add_includedirs("src", "include")

-- Offline check of the Java-pack baker (src/core/java) against tools/convert_java_pack.py.
-- Build: xmake build bake_test
target("bake_test")
    set_kind("binary")
    set_default(false)
    set_languages("c++20")
    add_cxflags("/utf-8", "/EHs")
    add_defines("NOMINMAX", "UNICODE")
    add_files("tools/bake_test/main.cpp", "src/core/java/*.cpp", "src/core/item/*.cpp")
    add_includedirs("src")
    add_packages("nlohmann_json", "stb", "zlib")

-- Pack author tool (no game needed): check / preview / pack. See docs/tool.md.
-- Build: xmake build tessera_tool
target("tessera_tool")
    set_kind("binary")
    set_default(false)
    set_basename("tessera-tool")
    set_languages("c++20")
    add_cxflags("/utf-8", "/EHs")
    add_defines("NOMINMAX", "UNICODE")
    add_files("tools/tessera_tool/main.cpp", "src/core/java/*.cpp", "src/core/item/*.cpp", "src/core/ctm/*.cpp")
    add_includedirs("src")
    add_packages("nlohmann_json", "stb", "zlib")
    add_syslinks("shell32")
