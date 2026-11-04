-- Shared by Home and Agent. Jobs describe top-level completion, not spawned tasks.
local service = {jobs={}, serial=0}
local function text(value)
    local ok, result = pcall(tostring, value)
    return (ok and result or "<tostring failed>"):sub(1, 2048)
end
function service.start(code, origin)
    assert(type(code) == "string" and code:match("%S"), "No code to run")
    assert(#code <= context.maxBytes, "Script exceeds the editor limit")
    local active = 0
    for _, job in service.jobs do if job.status == "running" then active += 1 end end
    assert(active < 4, "Four scripts are already running")
    service.serial += 1
    local job = {id="run-" .. service.serial, origin=origin or "editor", status="compiling", output={}, returns={}, started=os.clock()}
    service.jobs[#service.jobs + 1] = job
    if #service.jobs > 32 then
        for index, old in service.jobs do
            if old.status ~= "running" then table.remove(service.jobs, index); break end
        end
    end
    local compiled, problem = context.compile(code, "@basalt-" .. job.origin .. "-" .. job.id)
    if not compiled then
        job.status, job.error, job.finished = "compile_error", text(problem), os.clock()
        context.reportError("[compile] " .. job.error)
        return job
    end
    local inherited = getfenv(compiled)
    local env = setmetatable({}, {__index=inherited})
    local function printer(native, prefix, ...)
        local values = table.pack(...)
        local strings = {}
        for index = 1, values.n do strings[index] = text(values[index]) end
        job.output[#job.output + 1] = prefix .. table.concat(strings, " "):sub(1, 4096)
        if #job.output > 60 then table.remove(job.output, 1) end
        native(...)
    end
    local function capture(native, prefix)
        local replacement = function(...) printer(native, prefix, ...) end
        if type(clonefunction) == "function" and type(hookfunction) == "function" and
            type(iscclosure) == "function" and iscclosure(native) then
            local proxy = clonefunction(native)
            hookfunction(proxy, replacement)
            return proxy
        end
        return replacement
    end
    env.print = capture(context.print, "")
    env.warn = capture(context.warn, "[warn] ")
    env.loadstring = function(source, name)
        local fn, problem = context.compile(source, name)
        if fn then setfenv(fn, env) end
        return fn, problem
    end
    setfenv(compiled, env)
    job.status = "running"
    task.spawn(function()
        -- Native functions observe the coroutine's globals; keep them aligned
        -- with this job's Lua environment while getgenv remains session-wide.
        setfenv(0, env)
        local results = table.pack(xpcall(compiled, debug.traceback))
        job.finished = os.clock()
        if results[1] then
            job.status = "completed"
            for index = 2, math.min(results.n, 17) do job.returns[#job.returns + 1] = text(results[index]) end
            if #job.returns > 0 then context.print("[return] " .. table.concat(job.returns, "  ")) end
        else
            job.status, job.error = "runtime_error", text(results[2])
            context.reportError(job.error)
        end
    end)
    return job
end
function service.get(id)
    for _, job in service.jobs do
        if job.id == id then
            return {id=job.id, origin=job.origin, status=job.status, error=job.error,
                output=table.clone(job.output), returns=table.clone(job.returns),
                elapsed=math.floor(((job.finished or os.clock()) - job.started) * 1000) / 1000}
        end
    end
    error("Run is no longer available: " .. tostring(id))
end
return service
