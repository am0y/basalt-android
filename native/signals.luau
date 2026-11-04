return function(env)
    local records = setmetatable({}, {__mode = "k"})
    local methods = {}
    local function record(self)
        local value = records[self]
        assert(value, "expected a Connection")
        return value
    end
    function methods:Fire(...)
        local value = record(self)
        if value.ForeignState then return end
        if value.Once or value.Waiting then
            if not value.Handle.Connected then return end
            value.Handle:Disconnect()
        end
        if value.Waiting then task.spawn(value.Thread, ...)
        elseif value.Function then task.spawn(value.Function, ...) end
    end
    function methods:Defer(...)
        local value = record(self)
        if value.ForeignState then return end
        if value.Once or value.Waiting then
            if not value.Handle.Connected then return end
            value.Handle:Disconnect()
        end
        if value.Waiting then task.defer(value.Thread, ...)
        elseif value.Function then task.defer(value.Function, ...) end
    end
    function methods:Disable()
        env._setConnectionEnabled(record(self).Handle, false)
    end
    function methods:Enable()
        env._setConnectionEnabled(record(self).Handle, true)
    end
    function methods:Disconnect()
        local value = record(self)
        value.Handle:Disconnect()
        env._setConnectionEnabled(value.Handle, true)
    end
    local mt = {
        __index = function(self, key)
            local value = record(self)
            if key == "Enabled" then return value.Handle.Connected and env._connectionEnabled(value.Handle) end
            if key == "Function" or key == "Thread" or key == "ForeignState" or key == "LuaConnection" then
                return value[key]
            end
            return methods[key]
        end,
        __newindex = function() error("Connection fields are read-only", 2) end,
        __metatable = "Connection",
    }
    local function noop() end
    env.getconnections = function(signal)
        assert(typeof(signal) == "RBXScriptSignal", "expected an RBXScriptSignal")
        local temporary = signal:Connect(noop)
        local ok, snapshots = pcall(env._snapshotConnections, temporary)
        temporary:Disconnect()
        assert(ok, snapshots)
        local result = table.create(#snapshots)
        for index, value in snapshots do
            local connection = setmetatable({}, mt)
            records[connection] = value
            result[index] = connection
        end
        return result
    end
    env.firesignal = function(signal, ...)
        for _, connection in env.getconnections(signal) do
            if connection.Enabled then connection:Fire(...) end
        end
    end
end
