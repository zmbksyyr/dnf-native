-- Exercise the original game proxy before database/resource initialization.
local frida = require("df.frida")
assert(frida.load("channel-proxy") == 42, "original channel script acquisition")
print("dnf-native-original-channel-proxy-ok")
os.exit(42)
