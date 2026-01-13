Import("env")
import os

# Only run if custom_add_libs is defined
custom_add_libs = env.GetProjectOption("custom_add_libs", "")
if not custom_add_libs or not custom_add_libs.strip():
    # No conditional libs configured, skip entirely
    Return()

def parse_mapping(raw_value):
    mappings = []
    if raw_value:
        for line in raw_value.strip().split("\n"):
            line = line.strip()
            if "=" in line and not line.startswith(";"):
                flags_part, lib = line.split("=", 1)
                flags = {f.strip() for f in flags_part.split("|")}
                mappings.append((flags, lib.strip()))
    return mappings

def get_defined_flags(env):
    defined = set()
    build_flags = env.GetProjectOption("build_flags", [])
    if isinstance(build_flags, str):
        build_flags = build_flags.split("\n")
    for flag in build_flags:
        flag = str(flag).strip()
        if flag.startswith("-D"):
            defined.add(flag[2:].split("=")[0].strip())
    return defined

defined_flags = get_defined_flags(env)
add_libs = parse_mapping(custom_add_libs)

libs_to_add = []
libs_to_remove = []

for flags, lib in add_libs:
    matched = flags & defined_flags
    if matched:
        libs_to_add.append(lib)
    else:
        libs_to_remove.append(lib)

from platformio.package.manager.library import LibraryPackageManager
libdeps_dir = os.path.join(env["PROJECT_LIBDEPS_DIR"], env["PIOENV"])
lm = LibraryPackageManager(libdeps_dir)

changes_made = False

for lib in libs_to_add:
    try:
        pkg = lm.get_package(lib)
        if not pkg:
            lm.install(lib)
            print(f"[conditional_libs] +{lib}")
            changes_made = True
    except Exception as e:
        print(f"[conditional_libs] Install error for {lib}: {e}")

for lib in libs_to_remove:
    try:
        pkg = lm.get_package(lib)
        if pkg:
            lm.uninstall(pkg)
            print(f"[conditional_libs] -{lib}")
            changes_made = True
    except Exception:
        pass

if changes_made:
    ldf_cache = os.path.join(env["PROJECT_DIR"], ".pio", "build", env["PIOENV"], ".ldf_cache")
    if os.path.exists(ldf_cache):
        os.remove(ldf_cache)
    
    ide_cache = os.path.join(env["PROJECT_DIR"], ".pio", "build", env["PIOENV"], "idedata.json") 
    if os.path.exists(ide_cache):
        os.remove(ide_cache)
    
    print("[conditional_libs] Library changes detected - run build again.")
    print("[conditional_libs] Please wait for the metadata scanner to finish first!")
    import sys
    sys.exit(1)