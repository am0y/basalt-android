return function(env, shared)
    local overrides = shared.__basalt_property_overrides
    if not overrides then
        overrides = setmetatable({}, {__mode="k"})
        shared.__basalt_property_overrides = overrides
    end
    local default, sameInstance = env._propertyDefault, env.compareinstances
    local nativeRead = env.gethiddenproperty
    local function enumerate(object, hiddenOnly)
        assert(typeof(object) == "Instance", "expected an Instance")
        local result, unavailable = {}, {}
        for _, descriptor in env._getPropertyDescriptors(object) do
            if hiddenOnly and descriptor.Scriptable then continue end
            local ok, value = false, nil
            if descriptor.Scriptable then
                ok, value = pcall(function() return object[descriptor.Name] end)
            end
            if not ok or value == nil then
                ok, value = pcall(env.gethiddenproperty, object, descriptor.Name)
            end
            if ok then result[descriptor.Name] = value else unavailable[descriptor.Name] = tostring(value) end
        end
        return result, unavailable
    end
    env.getproperties = function(object) return enumerate(object, false) end
    env.gethiddenproperties = function(object) return enumerate(object, true) end
    env.gethiddenproperty = function(object, key)
        local value, hidden = nativeRead(object, key)
        if type(value) == "number" and type(key) == "string" then
            local category = key:gsub("Raw$", "")
            category = category:sub(1, 1):upper() .. category:sub(2)
            local exists, enum = pcall(function() return Enum[category] end)
            if exists and enum then
                for _, item in enum:GetEnumItems() do
                    if item.Value == value then return item, hidden end
                end
            end
        end
        return value, hidden
    end
    local function properties(object)
        local values = overrides[object]
        if values then return values end
        for owner, candidate in overrides do
            if sameInstance(owner, object) then
                overrides[object] = candidate
                return candidate
            end
        end
        return nil
    end
    env._getPropertyOverride = function(object, key)
        if typeof(object) ~= "Instance" or type(key) ~= "string" then return nil end
        local values = properties(object)
        if values then return values[key] end
        return nil
    end
    env.isscriptable = function(object, key)
        local original = default(object, key)
        if original == nil then return nil end
        local values = properties(object)
        if values and values[key] ~= nil then return values[key] end
        return original
    end
    env.setscriptable = function(object, key, state)
        assert(type(state) == "boolean", "expected a scriptability boolean")
        local previous = env.isscriptable(object, key)
        if previous == nil then return nil end
        local values = properties(object)
        if not values then
            values = {}
            overrides[object] = values
        end
        if state == default(object, key) then values[key] = nil else values[key] = state end
        return previous
    end
end
