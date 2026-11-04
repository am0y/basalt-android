local ui = assert(getgenv().basaltEditor)
local installed = assert(getgenv().__basalt_instance_http, "Instance HTTP hook was not installed")
local mt = getrawmetatable(game)
local previous, readonly = mt.__namecall, isreadonly(mt)
local original
for _, upvalue in debug.getupvalues(installed.namecall) do
    if type(upvalue) == "function" and iscclosure(upvalue) and not isexecutorclosure(upvalue) then
        original = upvalue
        break
    end
end
assert(original and original ~= previous, "native Roblox Instance dispatch was not found")
local function replace(value)
    if readonly then setreadonly(mt, false) end
    rawset(mt, "__namecall", value)
    if readonly then setreadonly(mt, true) end
end
local ok, problem = xpcall(function()
    replace(original)
    local valid, reason = pcall(function() game:GetService("MessageBusService"):Publish() end)
    assert(not valid and tostring(reason):find("Basalt security:", 1, true),
        "unhooking the Lua metamethod bypassed the native guard: " .. tostring(reason))
    local rawHttp, rawProblem = pcall(function()
        return game:HttpGet("https://economy.roblox.com/v1/user/currency")
    end)
    assert(not rawHttp and tostring(rawProblem):find("Basalt security:", 1, true),
        "unhooking the Lua HTTP method reached Roblox's native transport")
    local saved = getthreadidentity()
    setthreadidentity(7)
    local escalated, message = pcall(function() game:GetService("HttpRbxApiService"):GetAsync() end)
    setthreadidentity(saved)
    assert(not escalated and tostring(message):find("Basalt security:", 1, true),
        "elevated thread identity bypassed the native guard")
    local source = assert(loadstring("return function() return game:GetService('Players'):ReportAbuseV3() end"))()
    setfenv(source, getrenv())
    local escaped, detail = pcall(source)
    assert(not escaped and tostring(detail):find("Basalt security:", 1, true),
        "switching to the game environment bypassed the native guard")
end, debug.traceback)
replace(previous)
ui.open()
assert(ok, problem)
print("SECURITY_BYPASS_OK", "native dispatch", "identity", "game environment")
