Import("env")
from datetime import datetime

version = datetime.now().strftime("%y%m%d") + "0"
env.Append(CPPDEFINES=[("VERSION", version)])
print(f"Build VERSION: {version}")