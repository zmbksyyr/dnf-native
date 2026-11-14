-- Run while the derived game plugin is still alive. Its base destructor runs
-- luv once before closing handles, which can otherwise invoke overdue scripts.
local uv = require("luv")
_DP.add_unload_handler(function()
    uv.walk(function(handle)
        if not uv.is_closing(handle) then uv.close(handle) end
    end)
    uv.run()
end)
