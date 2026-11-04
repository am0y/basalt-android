local function initialize(f, done, emit, compile, sharedEnvironment)
    local env = getfenv(f)
    local denyService = install_security(env)
    local roblox = getmetatable(sharedEnvironment).__index
    local actorRuntime = sharedEnvironment.__basalt_actor_runtime
    local nativePrint, nativeWarn, nativeError = roblox.print, roblox.warn, roblox.error
    local console = install_console(env, sharedEnvironment, compile, f, emit ~= nil, actorRuntime)
    local messages = emit and env.cloneref(game:GetService("LogService")).MessageOut:Connect(function(text, kind)
        local prefix = kind == Enum.MessageType.MessageWarning and "[warn] " or
            kind == Enum.MessageType.MessageError and "[error] " or ""
        emit(prefix .. text)
    end)
    local function write(kind, ...)
        local values = table.pack(...)
        for i = 1, values.n do
            local ok, text = pcall(tostring, values[i])
            values[i] = ok and text or "<tostring failed>"
        end
        local text = table.concat(values, "\t", 1, values.n)
        if kind == "error" then
            console.publish(text, Enum.MessageType.MessageError)
            if emit or actorRuntime then task.defer(nativeError, text, 0) end
        elseif kind == "warn" then
            env.warn(text)
        else
            env.print(text)
        end
    end
    local function output(...) write("print", ...) end
    env._reportScriptError = function(...) write("error", ...) end
    local nativeRequire = roblox.require
    env.require = env.newcclosure(function(module)
        local previous = env.getthreadidentity()
        env.setthreadidentity(2)
        local result = table.pack(pcall(nativeRequire, module))
        env.setthreadidentity(previous)
        if not result[1] then error(result[2], 2) end
        return table.unpack(result, 2, result.n)
    end)
    if not actorRuntime then env._createMobileUI = install_mobile_ui end
    env.debug = table.clone(debug)
    for name, implementation in env._debugNative do
        env.debug[name] = implementation
    end
    env._debugNative = nil
    install_encoding(env)
    install_extended(env)
    install_signals(env)
    if env._propertyDefault then install_properties(env, sharedEnvironment) end
    install_websocket(env)
    install_drawing(env, sharedEnvironment)
    env.hookmetamethod = function(object, name, replacement)
        assert(type(name) == "string" and type(replacement) == "function", "expected a method name and function")
        local mt = env.getrawmetatable(object)
        assert(mt and type(mt[name]) == "function", "metamethod is not a function")
        if typeof(object) == "Instance" then
            -- Engine-invoked metamethods must be plain Luau frames; a native
            -- newcclosure boundary here corrupts the caller's stack.
            replacement = env._unwrapCclosure(replacement)
            local previous = mt[name]
            local readonly = env.isreadonly(mt)
            if readonly then env.setreadonly(mt, false) end
            local ok, problem = pcall(rawset, mt, name, replacement)
            if readonly then env.setreadonly(mt, true) end
            assert(ok, problem)
            return previous
        end
        return env.hookfunction(mt[name], replacement)
    end
    env.getgenv = function() return sharedEnvironment end
    env.getrenv = function()
        local runtime = getmetatable(sharedEnvironment).__index
        if rawget(runtime, "_G") == nil then rawset(runtime, "_G", {}) end
        return runtime
    end
    local HttpService = env.cloneref(game:GetService("HttpService"))
    local reportedName = "Basalt"
    local readOk, encoded = pcall(env.readfile, ".basalt/settings.json")
    if readOk and type(encoded) == "string" and #encoded <= 4096 then
        local decodeOk, saved = pcall(function() return HttpService:JSONDecode(encoded) end)
        local name = decodeOk and type(saved) == "table" and type(saved.preferences) == "table" and saved.preferences.executorName
        if type(name) == "string" and #name > 0 and #name <= 32 and not name:find("[^%g ]") then
            reportedName = name
        end
    end
    env.identifyexecutor = function() return sharedEnvironment.__basalt_executor_name or reportedName, "1.2.4" end
    env.getexecutorname = env.identifyexecutor
    env.gethui = function()
        local current = sharedEnvironment.__basalt_hui
        if current and current.Parent then return current end
        local previous = env.getthreadidentity()
        env.setthreadidentity(8)
        local ok, container = pcall(function()
            local folder = Instance.new("Folder")
            -- Keep Basalt's host separate from Roblox's own CoreGui folders.
            folder.Name = "Basalt_UI"
            folder.Parent = env.cloneref(game:GetService("CoreGui"))
            -- Return the canonical wrapper: a cloneref proxy breaks
            -- child.Parent == gethui() identity checks.
            return folder
        end)
        env.setthreadidentity(previous)
        assert(ok, container)
        sharedEnvironment.__basalt_hui = container
        return container
    end
    env.loadfile = function(path)
        local ok, source = pcall(env.readfile, path)
        if not ok then return nil, source end
        return env.loadstring(source, "@" .. path)
    end
    env.dofile = function(path)
        local compiled, err = env.loadfile(path)
        assert(compiled, err)
        return compiled()
    end
    env.request = function(options)
        assert(type(options) == "table", "request expects a table")
        local payload = table.clone(options)
        -- Android blocks cleartext traffic; upgrade http:// so legacy scripts keep working.
        if type(payload.Url) == "string" and payload.Url:sub(1, 7):lower() == "http://" then
            payload.Url = "https://" .. payload.Url:sub(8)
        end
        -- setup.roblox.com's TLS certificate is issued for s3.amazonaws.com, so the
        -- upgraded request fails hostname verification; setup.rbxcdn.com serves the same files.
        if type(payload.Url) == "string" then
            local host, rest = payload.Url:match("^[Hh][Tt][Tt][Pp][Ss]://[Ss][Ee][Tt][Uu][Pp]%.[Rr][Oo][Bb][Ll][Oo][Xx]%.[Cc][Oo][Mm](.*)$")
            if host ~= nil then payload.Url = "https://setup.rbxcdn.com" .. host end
        end
        if payload.Body ~= nil then
            assert(type(payload.Body) == "string", "Body must be a string")
            payload.BodyBase64 = env.base64encode(payload.Body)
            payload.Body = nil
        end
        local id, problem = env._requestStart(HttpService:JSONEncode(payload))
        assert(id, problem)
        local function decode(body, errorText)
            assert(body, errorText)
            local result = HttpService:JSONDecode(body)
            assert(not result.error, result.error)
            if result.BodyBase64 then result.Body = env.base64decode(result.BodyBase64); result.BodyBase64 = nil end
            return result
        end
        while true do
            local ready, body, errorText = env._requestPoll(id)
            if ready then return decode(body, errorText) end
            local ok, waitError = pcall(task.wait, 0.05)
            if not ok then
                if tostring(waitError):find("thread is not yieldable", 1, true) then
                    local done, response, failure = env._requestAwait(id)
                    assert(done, failure or waitError)
                    return decode(response, failure)
                end
                env._requestCancel(id)
                error(waitError, 2)
            end
        end
    end
    env.http_request = env.request
    env.syn = {request = env.request}
    env.httpget = function(url, useCache)
        assert(type(url) == "string", "game:HttpGet requires a URL string")
        local result = env.request({Url = url, Method = "GET"})
        assert(result.Success, string.format("HTTP %d: %s", result.StatusCode, tostring(result.StatusMessage)))
        return result.Body
    end
    install_android_compat(env, sharedEnvironment)
    install_instance_http(env, sharedEnvironment, env.checkcaller, denyService)
    env.rconsoleprint = output
    env.rconsolewarn = env.warn
    env.rconsoleerr = env._reportScriptError
    env.rconsoleerror = env.rconsoleerr
    install_saveinstance(env)
    if not actorRuntime then install_input_compat(env) end
    install_actor_compat(env, sharedEnvironment)
    for name, value in env do
        if name ~= "script" and name ~= "_openDiscordInvite" and name ~= "_editorInput" and name ~= "_serviceProtection" then sharedEnvironment[name] = value end
    end
    if not actorRuntime then
        install_lifecycle(env, sharedEnvironment)
        sharedEnvironment.queue_on_teleport = env.queue_on_teleport
        sharedEnvironment.queueonteleport = env.queueonteleport
    end
    task.defer(function()
        local results = table.pack(xpcall(f, debug.traceback))
        if results[1] and results.n > 1 then
            output("[return]", table.unpack(results, 2, results.n))
        elseif not results[1] then
            env._reportScriptError(results[2])
        end
        task.wait() -- Flush deferred MessageOut deliveries before the completion marker.
        if done then done(table.unpack(results, 1, results.n)) end
        if messages then task.delay(30, function() messages:Disconnect() end) end
    end)
end

return function(...)
    local ok, problem = xpcall(initialize, debug.traceback, ...)
    if not ok then error("Basalt initialization failed: " .. tostring(problem), 0) end
end
