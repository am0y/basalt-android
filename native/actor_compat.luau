return function(env, shared)
    if shared.__basalt_actor_runtime then
        env.getactor = function()
            for _, instance in game:GetDescendants() do
                if instance:IsA("Actor") and env._instanceKey(instance) == shared.__basalt_actor_address then
                    return instance
                end
            end
            return nil
        end
    end
    env.getactors = function()
        local found = {}
        for _, instance in game:GetDescendants() do
            if instance:IsA("Actor") then found[#found+1] = instance end
        end
        return found
    end
    env.run_on_actor = function(actor, source, options)
        assert(typeof(actor) == "Instance" and actor:IsA("Actor"), "expected an Actor")
        assert(type(source) == "string" and source:match("%S") and #source <= 8 * 1024 * 1024,
            "expected 1 byte to 8 MiB of Luau source")
        assert(coroutine.isyieldable(), "run_on_actor requires a yieldable thread")
        assert(options == nil or type(options) == "table", "options must be a table")
        local timeout = options and options.Timeout or 15
        assert(type(timeout) == "number" and timeout >= 0.1 and timeout <= 20,
            "Actor timeout must be from 0.1 to 20 seconds")
        local id = env._queueActor(actor, source)
        local deadline = os.clock() + timeout
        repeat
            local ready, problem = env._pollActor(id)
            if ready ~= nil then assert(ready, problem); return true end
            task.wait(0.05)
        until os.clock() >= deadline
        local _, problem = env._cancelActor(id)
        error(problem or "Actor VM did not resume before the timeout", 2)
    end
    env.run_on_actors = function(source, options)
        local ran = 0
        for _, actor in env.getactors() do
            if pcall(env.run_on_actor, actor, source, options) then ran += 1 end
        end
        return ran
    end
    local channels = shared.__basalt_comm_channels or {}
    shared.__basalt_comm_channels = channels
    env.create_comm_channel = function()
        local id = game:GetService("HttpService"):GenerateGUID(false)
        local event = Instance.new("BindableEvent")
        channels[id] = event
        return id, event
    end
    env.get_comm_channel = function(id)
        assert(type(id) == "string", "channel id must be a string")
        local event = channels[id]
        assert(event and event.Parent == nil or event, "channel does not exist")
        return event
    end
    local function flagName(name)
        return (name:gsub("^D?F?FFlag", ""):gsub("^DFFlag", ""):gsub("^FInt", ""):gsub("^DFInt", ""):gsub("^FString", ""):gsub("^FLog", ""))
    end
    local function flags() return settings() end
    env.getfflag = function(name)
        assert(type(name) == "string", "expected a flag name")
        local clean = flagName(name)
        if clean == "TaskSchedulerTargetFps" and env.getfpscap then return tostring(env.getfpscap()) end
        local ok, value = pcall(function() return flags():GetFFlag(clean) end)
        if ok then return value end
        local ok2, value2 = pcall(function() return flags():GetFVariable(clean) end)
        assert(ok2, "flag is not defined: " .. name)
        return value2
    end
    env.getfflagtype = function(name)
        assert(type(name) == "string", "expected a flag name")
        local value = env.getfflag(name)
        if value == "true" or value == "false" or type(value) == "boolean" then return "bool" end
        if type(value) == "number" or (type(value) == "string" and value:match("^%-?%d+$")) then return "int" end
        return "string"
    end
    env.setfflag = function(name, value)
        assert(type(name) == "string" and type(value) == "string", "expected a flag name and string value")
        local clean = flagName(name)
        if clean == "TaskSchedulerTargetFps" then
            assert(value:match("^%d+$"), "FPS flag requires a nonnegative integer")
            env.setfpscap(tonumber(value))
            return
        end
        flags():SetFFlag(clean, value)
    end
    env.replicatesignal = env.replicatesignal or function() error("replicatesignal is unavailable", 2) end
    env.replicateproperty = function(instance, property, value)
        assert(typeof(instance) == "Instance", "expected an Instance")
        assert(type(property) == "string", "expected a property name")
        local success, problem = pcall(env.sethiddenproperty or error, instance, property, value)
        if success then return true end
        local ok2, problem2 = pcall(function() instance[property] = value end)
        assert(ok2, tostring(problem2 or problem))
        return true
    end
    local hidden = shared.__basalt_stack_hidden or setmetatable({}, {__mode = "k"})
    shared.__basalt_stack_hidden = hidden
    env.setstackhidden = function(fn, state)
        assert(type(fn) == "function", "expected a function")
        hidden[fn] = state ~= false or nil
    end
    env.isstackhidden = function(fn)
        assert(type(fn) == "function", "expected a function")
        return hidden[fn] == true
    end
end
