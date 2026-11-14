-- Runs inside the original DP2/Lua library, before game database startup.
local frida = require("df.frida")
local seen = 0

local function from_js(i, f, s)
    assert(i >= 1001 and i <= 1032, "JS/Lua integer")
    assert(f == i - 1000 + 0.5, "JS/Lua float")
    assert(s == "js-to-lua-" .. (i - 1000), "JS/Lua string")
    seen = seen + 1
    return i * 3
end

assert(frida.load("bridge-main", from_js) == 1401, "Frida main return")
for i = 1, 32 do
    assert(frida.call(i, i + 0.25, "lua-to-js-" .. i) == i * 7, "Lua/JS return")
end
assert(seen == 32, "JS/Lua call count")
print("dnf-native-lua-js-roundtrip-ok calls=32")
os.exit(42)
