return function(env)
    local http = env.cloneref(game:GetService("HttpService"))
    local escapeMap = { ["&"]="&amp;", ["<"]="&lt;", [">"]="&gt;", ['"']="&quot;", ["'"]="&apos;" }
    local function xml(value)
        local escaped = tostring(value):gsub("[&<>\"'%c]", function(character)
            if escapeMap[character] then return escapeMap[character] end
            if character == "\n" or character == "\r" or character == "\t" then return character end
            return "�"
        end)
        return escaped
    end
    local function decimal(value)
        if type(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then
            error("non-finite property value")
        end
        return string.format("%.17g", value):gsub(",", ".")
    end
    local function element(tag, name, body)
        return "<" .. tag .. ' name="' .. xml(name) .. '">' .. body .. "</" .. tag .. ">"
    end
    local function component(tag, value)
        return "<" .. tag .. ">" .. decimal(value) .. "</" .. tag .. ">"
    end
    local function encode(name, value, reflectedType, referents)
        local kind = typeof(value)
        if kind == "boolean" then return element("bool", name, tostring(value)) end
        if kind == "number" then
            local tag = reflectedType == 2 and "int" or reflectedType == 3 and "int64" or
                reflectedType == 4 and "float" or "double"
            return element(tag, name, decimal(value))
        end
        if kind == "string" then
            if reflectedType == 30 then return element("BinaryString", name, env.base64encode(value)) end
            if reflectedType == 63 then return nil end -- SharedStrings require a separate MD5 table.
            return element("string", name, xml(value))
        end
        if kind == "EnumItem" then return element("token", name, decimal(value.Value)) end
        if kind == "Instance" then
            return element("Ref", name, referents[env._instanceKey(value)] or "null")
        end
        if kind == "Vector3" then
            return element("Vector3", name, component("X", value.X) .. component("Y", value.Y) .. component("Z", value.Z))
        end
        if kind == "Vector2" then
            return element("Vector2", name, component("X", value.X) .. component("Y", value.Y))
        end
        if kind == "Color3" then
            return element("Color3", name, component("R", value.R) .. component("G", value.G) .. component("B", value.B))
        end
        if kind == "BrickColor" then return element("int", name, decimal(value.Number)) end
        if kind == "UDim" then
            return element("UDim", name, component("S", value.Scale) .. component("O", value.Offset))
        end
        if kind == "UDim2" then
            return element("UDim2", name, component("XS", value.X.Scale) .. component("XO", value.X.Offset) ..
                component("YS", value.Y.Scale) .. component("YO", value.Y.Offset))
        end
        if kind == "CFrame" then
            local x,y,z,r00,r01,r02,r10,r11,r12,r20,r21,r22 = value:GetComponents()
            return element("CoordinateFrame", name, component("X",x) .. component("Y",y) .. component("Z",z) ..
                component("R00",r00) .. component("R01",r01) .. component("R02",r02) ..
                component("R10",r10) .. component("R11",r11) .. component("R12",r12) ..
                component("R20",r20) .. component("R21",r21) .. component("R22",r22))
        end
        if kind == "NumberRange" then
            return element("NumberRange", name, component("Min",value.Min) .. component("Max",value.Max))
        end
        if kind == "Rect" then
            return element("Rect2D", name, "<min>" .. component("X",value.Min.X) .. component("Y",value.Min.Y) ..
                "</min><max>" .. component("X",value.Max.X) .. component("Y",value.Max.Y) .. "</max>")
        end
        if kind == "NumberSequence" then
            local points = {}
            for _, key in value.Keypoints do
                points[#points+1] = decimal(key.Time) .. " " .. decimal(key.Value) .. " " .. decimal(key.Envelope)
            end
            return element("NumberSequence", name, table.concat(points, " "))
        end
        if kind == "ColorSequence" then
            local points = {}
            for _, key in value.Keypoints do
                points[#points+1] = decimal(key.Time) .. " " .. decimal(key.Value.R) .. " " ..
                    decimal(key.Value.G) .. " " .. decimal(key.Value.B) .. " 0"
            end
            return element("ColorSequence", name, table.concat(points, " "))
        end
        if kind == "PhysicalProperties" then
            return element("PhysicalProperties", name, "<CustomPhysics>true</CustomPhysics>" ..
                component("Density",value.Density) .. component("Friction",value.Friction) ..
                component("Elasticity",value.Elasticity) .. component("FrictionWeight",value.FrictionWeight) ..
                component("ElasticityWeight",value.ElasticityWeight))
        end
        return nil
    end

    local services = {
        Workspace=true, Lighting=true, ReplicatedStorage=true, ReplicatedFirst=true,
        StarterGui=true, StarterPack=true, StarterPlayer=true, SoundService=true,
        Teams=true, TextChatService=true,
    }
    local function arguments(first, second)
        local options = {}
        if type(first) == "table" then options = first
        elseif typeof(first) == "Instance" then options.Root = first
        elseif type(first) == "string" then options.FilePath = first
        elseif first ~= nil then error("saveinstance expects an Instance, path, or options table", 3) end
        if type(second) == "string" then options.FilePath = second
        elseif type(second) == "table" then
            local merged = table.clone(second)
            merged.Root = options.Root or merged.Root
            options = merged
        elseif second ~= nil then error("invalid saveinstance options", 3) end
        local root = options.Root or options.Object or game
        assert(root == game or typeof(root) == "Instance", "Root must be game or an Instance")
        local roots = options.Objects or (root == game and game:GetChildren() or {root})
        assert(type(roots) == "table", "Objects must be an array of Instances")
        local selected = {}
        for _, instance in roots do
            assert(typeof(instance) == "Instance", "Objects must contain Instances")
            if options.Objects ~= nil or root ~= game or services[instance.ClassName] then
                selected[#selected+1] = instance
            end
        end
        assert(#selected > 0, "no exportable Instances")
        local isPlace = root == game and options.Objects == nil
        local filename = options.FilePath or options.Path or
            ("BasaltExports/" .. (isPlace and "place-" or "model-") .. tostring(game.PlaceId) .. "-" ..
                http:GenerateGUID(false):gsub("%-", "") .. (isPlace and ".rbxlx" or ".rbxmx"))
        assert(type(filename) == "string" and filename:lower():match("%.rbx[ml]x$"),
            "FilePath must end in .rbxlx or .rbxmx")
        return selected, filename, options
    end

    env.saveinstance = function(first, second)
        local roots, path, options = arguments(first, second)
        local limit = options.MaxInstances or 25000
        assert(type(limit) == "number" and limit == math.floor(limit) and limit >= 1 and limit <= 100000,
            "MaxInstances must be an integer from 1 to 100000")
        local nodes, byKey, top = {}, {}, {}
        local pending = {}
        for _, root in roots do pending[#pending+1] = {instance=root, parent=nil, root=true} end
        local index = 1
        while index <= #pending do
            local current = pending[index]
            index += 1
            local instance = current.instance
            if not current.root and not options.IncludeNonArchivable and not instance.Archivable then continue end
            local key = env._instanceKey(instance)
            if byKey[key] then continue end
            assert(#nodes < limit, "saveinstance exceeded MaxInstances")
            local record = {instance=instance, key=key, parent=current.parent, children={},
                referent="RBX" .. tostring(#nodes+1)}
            byKey[key], nodes[#nodes+1] = record, record
            if current.parent then current.parent.children[#current.parent.children+1] = record
            else top[#top+1] = record end
            for _, child in instance:GetChildren() do
                pending[#pending+1] = {instance=child, parent=record}
            end
            if #nodes % 500 == 0 then task.wait() end
        end
        local referents = {}
        for _, record in nodes do referents[record.key] = record.referent end
        local temp = ".basalt/export-" .. http:GenerateGUID(false):gsub("%-", "") .. ".rbxmx"
        if not env.isfolder(".basalt") then env.makefolder(".basalt") end
        local pendingText, pendingBytes, skipped, written = {}, 0, 0, 0
        local function append(text)
            pendingText[#pendingText+1] = text
            pendingBytes += #text
            if pendingBytes >= 65536 then
                env.appendfile(temp, table.concat(pendingText))
                table.clear(pendingText)
                pendingBytes = 0
            end
        end
        local function export()
            env.writefile(temp, '<roblox xmlns:xmime="http://www.w3.org/2005/05/xmlmime" version="4">\n' ..
                '<External>null</External><External>nil</External>\n')
            local stack = {}
            for i=#top,1,-1 do stack[#stack+1] = top[i] end
            while #stack > 0 do
                local record = table.remove(stack)
                if record == false then append('</Item>\n'); continue end
                append('<Item class="' .. xml(record.instance.ClassName) .. '" referent="' .. record.referent .. '"><Properties>\n')
                local properties = env._getPropertyDescriptors(record.instance)
                for _, descriptor in properties do
                    local name = descriptor.Name
                    if name == "Parent" or name == "ClassName" or name == "className" or
                        name == "Source" or name == "ScriptGuid" or name == "archivable" or
                        name == "UniqueId" or name == "HistoryId" or name == "DataCost" or
                        name == "NumExpectedDirectChildren" or name == "numExpectedDirectChildren" or
                        name == "Attributes" or name == "AttributesReplicate" or
                        name == "PropertyStatusStudio" then continue end
                    local ok, value = false, nil
                    if descriptor.Scriptable then
                        ok, value = pcall(function() return record.instance[name] end)
                    end
                    if not ok or value == nil then
                        ok, value = pcall(env.gethiddenproperty, record.instance, name)
                    end
                    if not ok then skipped += 1; continue end
                    local converted, encoded = pcall(encode, name, value, descriptor.Type, referents)
                    if converted and encoded then append(encoded .. "\n"); written += 1
                    else skipped += 1 end
                end
                if record.instance:IsA("LuaSourceContainer") then
                    local source
                    if options.Decompile then
                        local ok, result = pcall(env.decompile, record.instance)
                        if ok then source = result else skipped += 1 end
                    end
                    append(element("ProtectedString", "Source", source and xml(source) or "") .. "\n")
                end
                append('</Properties>\n')
                stack[#stack+1] = false
                for i=#record.children,1,-1 do stack[#stack+1] = record.children[i] end
                if written % 500 == 0 then task.wait() end
            end
            append('</roblox>\n')
            if pendingBytes > 0 then env.appendfile(temp, table.concat(pendingText)) end
            env.renamefile(temp, path)
        end
        local ok, problem = xpcall(export, debug.traceback)
        if not ok then pcall(env.delfile, temp); error(problem, 2) end
        return path, {Instances=#nodes, Properties=written, Skipped=skipped}
    end
end
