return function(env, shared)
    if shared.__basaltLifecycle then
        env.queue_on_teleport = shared.__basaltLifecycle.queue
        env.queueonteleport = env.queue_on_teleport
        env.syn.queue_on_teleport = env.queue_on_teleport
        env.clearteleportqueue = shared.__basaltLifecycle.clear
        shared.clearteleportqueue = env.clearteleportqueue
        return
    end
    local http = env.cloneref(game:GetService("HttpService"))
    local queuePath = "basalt/teleport-queue.json"
    local pending, total = {}, 0
    local state = {}
    shared.__basaltLifecycle = state
    function state.queue(source)
        assert(type(source) == "string" and source:match("%S"), "queue_on_teleport expects source code")
        assert(#pending < 64 and total + #source <= 8 * 1024 * 1024, "teleport queue exceeds 64 scripts or 8 MiB")
        if not env.isfolder("basalt") then env.makefolder("basalt") end
        local nextQueue = table.clone(pending)
        nextQueue[#nextQueue + 1] = source
        env.writefile(queuePath, http:JSONEncode(nextQueue))
        pending, total = nextQueue, total + #source
    end
    function state.clear()
        if env.isfile(queuePath) then env.delfile(queuePath) end
        table.clear(pending)
        total = 0
    end
    env.clearteleportqueue = state.clear
    shared.clearteleportqueue = state.clear
    env.queue_on_teleport = state.queue
    env.queueonteleport = state.queue
    env.syn.queue_on_teleport = state.queue
    local function launch(source, name)
        local compiled, problem = env.loadstring(source, "@" .. name)
        if not compiled then env._reportScriptError(name .. ": " .. tostring(problem)); return end
        setfenv(compiled, shared)
        task.defer(function()
            local ok, failure = xpcall(compiled, debug.traceback)
            if not ok then env._reportScriptError(name .. ": " .. tostring(failure)) end
        end)
    end
    -- Claim the old queue synchronously, before new scripts can enqueue work.
    local claimed
    local ok, problem = pcall(function()
        if env.isfile(queuePath) then
            local serialized = env.readfile(queuePath)
            env.delfile(queuePath)
            assert(#serialized <= 48 * 1024 * 1024, "saved teleport queue is too large")
            claimed = http:JSONDecode(serialized)
            assert(type(claimed) == "table" and #claimed <= 64, "invalid saved teleport queue")
        end
    end)
    if not ok then claimed = nil; env._reportScriptError("Teleport queue: " .. tostring(problem)) end
    task.defer(function()
        if claimed then
            local size = 0
            for index, source in ipairs(claimed) do
                if type(source) ~= "string" then env._reportScriptError("Invalid queued script"); break end
                size += #source
                if size > 8 * 1024 * 1024 then env._reportScriptError("Saved teleport queue exceeds 8 MiB"); break end
                launch(source, "teleport-" .. index)
            end
        end
        local ready, failure = pcall(function()
            local folder="basalt/autoexec"
            if not env.isfolder("basalt") then env.makefolder("basalt") end
            if not env.isfolder(folder) then env.makefolder(folder) end
            if env.isfolder("autoexec") then
                local old=env.listfiles("autoexec")
                table.sort(old)
                for _,path in old do
                    local name=path:match("^autoexec/([^/]+)$")
                    if name and name~="basalt-ui-preview.luau" and name~="basalt-autoexec-bridge.luau" and
                        (name:match("%.lua$") or name:match("%.luau$")) and env.isfile(path) then
                        local target=folder.."/"..name
                        if env.isfile(target) then
                            env._reportScriptError("Autoexec migration: duplicate "..target)
                        else
                            env.renamefile(path,target)
                        end
                    end
                end
            end
            local enabled=true
            local loaded,serialized=pcall(env.readfile,".basalt/settings.json")
            if loaded and type(serialized)=="string" and #serialized<=4096 then
                local decoded,settings=pcall(function() return http:JSONDecode(serialized) end)
                if decoded and type(settings)=="table" and type(settings.preferences)=="table" and
                    settings.preferences.autoexecute==false then enabled=false end
            end
            if enabled then
                local files=env.listfiles(folder)
                table.sort(files)
                for _,path in files do
                    if (path:match("%.lua$") or path:match("%.luau$")) and env.isfile(path) then
                        local read,source=pcall(env.readfile,path)
                        if read then launch(source,path) else env._reportScriptError(path..": "..tostring(source)) end
                    end
                end
            end
        end)
        if not ready then env._reportScriptError("Autoexec: " .. tostring(failure)) end
    end)
end
