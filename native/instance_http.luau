return function(env, shared, ownsCall, denyService)
    if shared.__basalt_instance_http then return end
    local dataModel = game
    env._disableInstanceDirectAccess(dataModel)
    local mt = env.getrawmetatable(dataModel)
    local originalIndex, originalNamecall, originalNewindex = mt.__index, mt.__namecall, mt.__newindex
    local propertyOverride = env._getPropertyOverride
    local readProperty, writeProperty = env.gethiddenproperty, env.sethiddenproperty
    local sameInstance, namecallMethod = env.compareinstances, env.getnamecallmethod
    local fetch = env.httpget
    local getService = originalIndex(dataModel, "GetService")
    local findService = originalIndex(dataModel, "FindService")
    local function isService(self, key)
        return (key == "GetService" or key == "getService" or key == "service") and
            typeof(self) == "Instance" and sameInstance(self, dataModel)
    end
    local function service(self, name, ...)
        -- The client engine throws an unhandled C++ exception when it creates
        -- NetworkServer beside NetworkClient. Convert this verified crash path
        -- into a catchable script error, only for executor-owned calls.
        if name == "NetworkServer" and ownsCall and ownsCall() and
            sameInstance(self, dataModel) and findService(dataModel, "NetworkClient") then
            error("NetworkServer cannot be created while NetworkClient is present", 2)
        end
        return getService(self, name, ...)
    end
    local function isHttp(self, key)
        return (key == "HttpGet" or key == "HttpGetAsync") and
            typeof(self) == "Instance" and sameInstance(self, dataModel)
    end
    local function method(self, url, useCache)
        assert(not ownsCall or ownsCall(), "HTTP compatibility methods require an executor call")
        assert(typeof(self) == "Instance" and sameInstance(self, dataModel), "HttpGet requires the DataModel")
        return fetch(url, useCache)
    end
    -- Keep ordinary Luau frames here: wrapping __index in newcclosure adds
    -- a non-yieldable native boundary to methods retrieved through the hook.
    local function index(self, key)
        if isHttp(self, key) and (not ownsCall or ownsCall()) then return method end
        if ownsCall and ownsCall() then
            denyService(self, key)
        end
        if ownsCall and isService(self, key) and ownsCall() then return service end
        local override = propertyOverride and propertyOverride(self, key)
        if override ~= nil and (not ownsCall or ownsCall()) then
            if override == false then error(tostring(key) .. " is not scriptable", 2) end
            return (readProperty(self, key))
        end
        return originalIndex(self, key)
    end
    local function newindex(self, key, value)
        local override = propertyOverride(self, key)
        if override ~= nil and (not ownsCall or ownsCall()) then
            if override == false then error(tostring(key) .. " is not scriptable", 2) end
            writeProperty(self, key, value); return
        end
        return originalNewindex(self, key, value)
    end
    local function namecall(self, ...)
        local key = namecallMethod()
        if isHttp(self, key) and (not ownsCall or ownsCall()) then return method(self, ...) end
        if ownsCall and ownsCall() then denyService(self, key) end
        if ownsCall and isService(self, key) and ownsCall() then return service(self, ...) end
        return originalNamecall(self, ...)
    end
    local readonly = env.isreadonly(mt)
    env.setreadonly(mt, false)
    local ok, problem = pcall(function()
        rawset(mt, "__index", index)
        rawset(mt, "__namecall", namecall)
        if propertyOverride then rawset(mt, "__newindex", newindex) end
    end)
    if not ok then
        rawset(mt, "__index", originalIndex)
        rawset(mt, "__namecall", originalNamecall)
        if propertyOverride then rawset(mt, "__newindex", originalNewindex) end
    end
    env.setreadonly(mt, readonly)
    assert(ok, problem)
    shared.__basalt_instance_http = {index=index, namecall=namecall, newindex=newindex, method=method}
end
