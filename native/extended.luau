return function(env)
    local info = debug.info
    env.debug.getregistry = env.getreg
    env.debug.getmetatable = env.getrawmetatable
    env.debug.setmetatable = env.setrawmetatable
    env.getidentity = env.getthreadidentity
    env.setidentity = env.setthreadidentity
    env.getthreadcontext = env.getthreadidentity
    env.setthreadcontext = env.setthreadidentity
    env.isourclosure = env.isexecutorclosure
    env.checkclosure = env.isexecutorclosure
    env.replaceclosure = env.hookfunction
    env.newlclosure = function(fn)
        assert(type(fn) == "function", "expected a function")
        return function(...) return fn(...) end
    end
    env.makereadonly = function(value) return env.setreadonly(value, true) end
    env.makewritable = function(value) return env.setreadonly(value, false) end
    local hooks = setmetatable({}, {__mode="k"})
    local contexts = setmetatable({}, {__mode="k"})
    env.oth = {}
    env.oth.get_original_thread = function()
        local context = contexts[coroutine.running()]
        return context and context.caller
    end
    env.oth.get_root_callback = function()
        local context = contexts[coroutine.running()]
        return context and context.original
    end
    env.oth.is_hook_thread = function() return contexts[coroutine.running()] ~= nil end
    env.oth.hook = function(target, callback)
        assert(type(target) == "function" and type(callback) == "function", "expected a target and callback")
        assert(not hooks[target], "function already has an OTH hook")
        local original
        original = env.hookfunction(target, function(...)
            local caller, finished, waiting, results = coroutine.running(), false, false, nil
            local worker = coroutine.create(function(...)
                contexts[coroutine.running()] = {caller=caller, original=original}
                results = table.pack(pcall(callback, ...))
                contexts[coroutine.running()] = nil
                finished = true
                if waiting then task.defer(caller) end
            end)
            task.spawn(worker, ...)
            if not finished then
                if not coroutine.isyieldable() then
                    task.cancel(worker)
                    contexts[worker] = nil
                    error("yielding OTH callback requires a yieldable caller", 2)
                end
                waiting = true
                while not finished do coroutine.yield() end
            end
            if not results[1] then error(results[2], 0) end
            return table.unpack(results, 2, results.n)
        end)
        hooks[target] = original
        return original
    end
    env.oth.unhook = function(target)
        assert(type(target) == "function", "expected a function")
        local original = hooks[target]
        if original then
            env.hookfunction(target, original)
            hooks[target] = nil
        end
    end
    env.debug.isvalidlevel = function(level)
        assert(type(level) == "number" and level >= 0 and level == math.floor(level), "invalid stack level")
        return info(level + 1, "f") ~= nil
    end
    env.debug.getcallstack = function()
        local result, level = {}, 2
        while true do
            local fn = info(level, "f")
            if not fn then break end
            local source, line, name = info(level, "sln")
            result[#result + 1] = {func=fn, source=source, line=line, name=name}
            level += 1
        end
        return result
    end
    env.getcallbackvalue = function(object, member)
        assert(typeof(object) == "Instance" and type(member) == "string", "expected an Instance and callback name")
        local class = object.ClassName
        assert((class == "BindableFunction" and member == "OnInvoke") or
            (class == "RemoteFunction" and member == "OnClientInvoke"), "unsupported callback property: " .. member)
        return env._callbackValue(object)
    end
    env.debug.getinfo = function(target, options)
        assert(type(target) == "function" or type(target) == "number", "expected a function or stack level")
        options = options or "sluanf"
        assert(type(options) == "string" and not options:find("[^Sslufan]"), "invalid debug.getinfo option")
        local fn = target
        if type(target) == "number" then
            assert(target >= 0 and target == math.floor(target), "invalid stack level")
            fn = info(target + 1, "f")
            if not fn then return nil end
        end
        local source, defined, name = info(fn, "sln")
        local parameters, variadic = info(fn, "a")
        local isC = env.iscclosure(fn)
        return {
            source = isC and "=[C]" or "@" .. source,
            short_src = source,
            what = isC and "C" or "Lua",
            name = name,
            namewhat = name ~= "" and "global" or "",
            currentline = type(target) == "number" and info(target + 1, "l") or defined,
            linedefined = defined,
            lastlinedefined = defined,
            nups = env._closureUpvalueCount(fn),
            numparams = parameters,
            nparams = parameters,
            is_vararg = variadic and 1 or 0,
            isvararg = variadic,
            func = fn,
        }
    end
    local function contains(values, wanted)
        for _, needle in wanted do
            local found = false
            for _, value in next, values do
                if rawequal(value, needle) or
                    (type(value) == "number" and type(needle) == "number" and value ~= value and needle ~= needle) then
                    found = true
                    break
                end
            end
            if not found then return false end
        end
        return true
    end
    env.filtergc = function(kind, options, returnOne)
        assert(kind == "table" or kind == "function", "filtergc expects table or function")
        assert(type(options) == "table", "filtergc expects an options table")
        assert(returnOne == nil or type(returnOne) == "boolean", "returnOne must be a boolean")
        local result = {}
        for _, value in env.getgc(kind == "table") do
            if type(value) ~= kind then continue end
            if value == options or value == options.KeyValuePairs or value == options.Keys or value == options.Values then continue end
            local matches = true
            if kind == "function" then
                if options.IgnoreExecutor ~= false and env.isexecutorclosure(value) then continue end
                if options.Name and debug.info(value, "n") ~= options.Name then continue end
                if options.Constants or options.Upvalues or options.Hash then
                    if env.iscclosure(value) then continue end
                    if options.Constants and not contains(env.debug.getconstants(value), options.Constants) then continue end
                    if options.Upvalues and not contains(env.debug.getupvalues(value), options.Upvalues) then continue end
                    if options.Hash then
                        assert(env.getfunctionhash, "getfunctionhash is unavailable")
                        if env.getfunctionhash(value) ~= options.Hash then continue end
                    end
                end
            else
                if options.Metatable and not rawequal(env.getrawmetatable(value), options.Metatable) then continue end
                if options.Keys then
                    for _, key in options.Keys do if rawget(value, key) == nil then matches = false; break end end
                end
                if matches and options.Values and not contains(value, options.Values) then matches = false end
                if matches and options.KeyValuePairs then
                    for key, expected in options.KeyValuePairs do
                        if not rawequal(rawget(value, key), expected) then matches = false; break end
                    end
                end
            end
            if matches then
                if returnOne then return value end
                result[#result + 1] = value
            end
        end
        if returnOne then return nil end
        return result
    end
    env.getinstances = function()
        local result = {}
        for _, value in env._getUserdata() do
            if typeof(value) == "Instance" then result[#result + 1] = value end
        end
        return result
    end
    env.getnilinstances = function()
        local result = {}
        for _, value in env.getinstances() do
            if value.Parent == nil then result[#result + 1] = value end
        end
        return result
    end
    env.getscripts = function()
        local result = {}
        for _, value in env.getinstances() do
            if value:IsA("LuaSourceContainer") then result[#result + 1] = value end
        end
        return result
    end
    env.getrunningscripts = function()
        local seen, result = {}, {}
        for _,thread in env._getThreads() do
            if type(thread)~="thread" then continue end
            local script=env.getscriptfromthread(thread)
            if typeof(script)=="Instance" and script:IsA("LuaSourceContainer") and not script:IsA("CoreScript") and not seen[script] then
                seen[script]=true
                result[#result+1]=script
            end
        end
        for _, value in env.getgc(false) do
            if type(value) ~= "function" or not env.islclosure(value) then continue end
            local ok, environment = pcall(getfenv, value)
            local script = ok and rawget(environment, "script")
            if typeof(script) == "Instance" and script:IsA("LuaSourceContainer") and not script:IsA("CoreScript") and not seen[script] then
                seen[script] = true
                result[#result + 1] = script
            end
        end
        return result
    end
    env.getloadedmodules = function()
        local result, seen = {}, {}
        for _,value in env.getinstances() do
            if value:IsA("ModuleScript") and not seen[value] and env._moduleLoaded(value) then
                seen[value]=true
                result[#result+1]=value
            end
        end
        return result
    end
    env.getsenv = function(script)
        assert(typeof(script) == "Instance" and script:IsA("LuaSourceContainer"), "expected a script")
        for _,thread in env._getThreads() do
            if type(thread)=="thread" and env.getscriptfromthread(thread)==script then
                return env._threadEnvironment(thread)
            end
        end
        for _, value in env.getgc(false) do
            if type(value) == "function" and env.islclosure(value) then
                local ok, environment = pcall(getfenv, value)
                if ok and rawget(environment, "script") == script then return environment end
            end
        end
        error("script has no accessible running environment", 2)
    end
    env.getscriptthread = function(script)
        assert(typeof(script) == "Instance" and script:IsA("LuaSourceContainer"), "expected a script")
        for _, thread in env._getThreads() do
            if type(thread) == "thread" and coroutine.status(thread) ~= "dead" and env.getscriptfromthread(thread) == script then
                return thread
            end
        end
        return nil
    end
    env.isourthread = function(thread)
        thread = thread or coroutine.running()
        assert(type(thread) == "thread", "expected a thread")
        if env.getscriptfromthread(thread) ~= nil then return false end
        local level = 1
        while true do
            local fn = info(thread, level, "f")
            if not fn then return false end
            if env.isexecutorclosure(fn) then return true end
            level += 1
        end
    end
    env.getconnection = function(signal, index)
        assert(type(index) == "number" and index == math.floor(index), "expected an integer index")
        local connections = env.getconnections(signal)
        return connections[index]
    end
    if env.getsignalargumentsinfo then
        env.getsignalarguments = function(signal)
            local result = {}
            for index, argument in env.getsignalargumentsinfo(signal) do result[index] = argument.Type end
            return result
        end
    end
    env.getproximitypromptduration = function(prompt)
        assert(typeof(prompt) == "Instance" and prompt:IsA("ProximityPrompt"), "expected a ProximityPrompt")
        return prompt.HoldDuration
    end
    env.setproximitypromptduration = function(prompt, duration)
        assert(typeof(prompt) == "Instance" and prompt:IsA("ProximityPrompt"), "expected a ProximityPrompt")
        assert(type(duration) == "number" and duration >= 0 and duration < math.huge, "expected a finite nonnegative duration")
        prompt.HoldDuration = duration
    end
    env.getsimulationradius = function()
        local player = game:GetService("Players").LocalPlayer
        assert(player, "LocalPlayer is unavailable")
        return (env.gethiddenproperty(player, "SimulationRadius"))
    end
    env.setsimulationradius = function(radius, maximum)
        assert(type(radius) == "number" and radius >= 0 and radius < math.huge, "expected a finite nonnegative radius")
        assert(maximum == nil or type(maximum) == "number" and maximum >= 0 and maximum < math.huge,
            "expected a finite nonnegative maximum radius")
        local player = game:GetService("Players").LocalPlayer
        assert(player, "LocalPlayer is unavailable")
        if maximum ~= nil then env.sethiddenproperty(player, "MaximumSimulationRadius", maximum) end
        env.sethiddenproperty(player, "SimulationRadius", radius)
    end
    env.getcallingscript = function()
        local level = 2
        while true do
            local fn = debug.info(level, "f")
            if not fn then break end
            local ok, environment = pcall(getfenv, fn)
            if ok then
                local script = rawget(environment, "script")
                if typeof(script) == "Instance" and script:IsA("LuaSourceContainer") then return script end
            end
            level += 1
        end
        local owner = env.getscriptfromthread(coroutine.running())
        if typeof(owner) == "Instance" then return owner end
        return nil
    end
end
