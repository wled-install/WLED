Import('env')

# Enable Link Time Optimization (-flto)
# Allows the compiler to optimize across compilation units at link time.
env.Append(
    CCFLAGS=["-flto"],
    CXXFLAGS=["-flto"],
    LINKFLAGS=["-flto"]
)

# Replace ar with gcc-ar so static library archives contain LTO bitcode
cc = env.get("CC", "")
if cc:
    gcc_ar = cc.replace("gcc", "gcc-ar").replace("g++", "gcc-ar")
    env.Replace(AR=gcc_ar)
