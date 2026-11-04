return function(env)
    local blocked = {}
    for _, rule in env._securityRules() do
        local methods = blocked[rule[1]] or {}
        methods[rule[2]:lower()] = true
        blocked[rule[1]] = methods
    end
    local function deny(self, method)
        if type(method) ~= "string" or typeof(self) ~= "Instance" then return end
        local class = self.ClassName
        local methods = blocked[class]
        if methods and methods[method:lower()] then
            error("Basalt security: " .. class .. "." .. method .. " is blocked for executor scripts", 3)
        end
    end
    local function cleanup()
        env._securityRegister, env._securityRules, env._securityNamecall = nil, nil, nil
        env._securityReady, env._securitySeal = nil, nil
    end
    if env._securityReady() then cleanup(); return deny end
    local register, rules, protectNamecall = env._securityRegister, env._securityRules, env._securityNamecall
    local mt = env.getrawmetatable(game)
    protectNamecall(mt.__namecall)
    for _, rule in rules() do
        local ok, service = pcall(function()
            return rule[1] == "DataModel" and game or game:GetService(rule[1])
        end)
        if ok and service then
            local available, method = pcall(function() return service[rule[2]] end)
            if available and type(method) == "function" and env.iscclosure(method) then
                register(rule[1], rule[2], method)
            end
        end
    end
    env._securitySeal()
    cleanup()
    return deny
end
