return function(env, shared)
    local registerAsset = env.getcustomasset
    local audioAssets = shared.__basaltAudioAssets or {items={}, order={}}
    shared.__basaltAudioAssets = audioAssets
    env.getcustomasset = function(path)
        local uri = registerAsset(path)
        local extension = path:lower():match("%.([%w]+)$")
        if ({mp3=true, ogg=true, wav=true, flac=true, aac=true, m4a=true})[extension] and
            not audioAssets.items[uri] and coroutine.isyieldable() then
            -- Android's audio decoder initializes asynchronously. Prime it
            -- before returning so immediate Sound:Play calls start reliably.
            local sound = Instance.new("Sound")
            sound.Name = "BasaltAudioCache"
            sound.Volume = 0
            sound.SoundId = uri
            sound.Parent = game:GetService("SoundService")
            pcall(function() game:GetService("ContentProvider"):PreloadAsync({sound}) end)
            if sound.IsLoaded and not audioAssets.items[uri] then
                audioAssets.items[uri] = sound
                table.insert(audioAssets.order, uri)
                if #audioAssets.order > 32 then
                    local oldest = table.remove(audioAssets.order, 1)
                    if audioAssets.items[oldest] then audioAssets.items[oldest]:Destroy() end
                    audioAssets.items[oldest] = nil
                end
            else sound:Destroy() end
        end
        return uri
    end
    env.fireclickdetector = function(detector, distance, event)
        assert(typeof(detector) == "Instance" and detector:IsA("ClickDetector"), "expected a ClickDetector")
        distance = distance == nil and 0 or distance
        event = event or "MouseClick"
        assert(type(distance) == "number" and distance >= 0 and distance <= 1e9, "invalid click distance")
        local kind = ({MouseClick=0, MouseHoverEnter=1, MouseHoverLeave=2, RightMouseClick=3})[event]
        assert(kind, "unsupported ClickDetector event")
        local player = game:GetService("Players").LocalPlayer
        assert(player, "LocalPlayer is unavailable")
        env._fireClickDetector(detector, player, distance, kind)
    end
    local http = env.cloneref(game:GetService("HttpService"))
    local request = env.request
    env.request = function(options)
        assert(type(options) == "table", "request expects a table")
        local payload = table.clone(options)
        if not payload.BridgeOperation then
            assert(payload.Headers == nil or type(payload.Headers) == "table", "Headers must be a table")
            local headers = table.clone(payload.Headers or {})
            local agent, fingerprint = false, false
            for name in headers do
                assert(type(name) == "string", "header names must be strings")
                agent = agent or name:lower() == "user-agent"
                fingerprint = fingerprint or name:lower() == "basalt-fingerprint"
            end
            if not agent then headers["User-Agent"] = "Basalt/1.2.2 (Android ARM64)" end
            if not fingerprint then headers["Basalt-Fingerprint"] = env.gethwid() end
            payload.Headers = headers
        end
        return request(payload)
    end
    env.http_request = env.request
    env.http = {request=env.request}
    env.syn.request = env.request
    env.syn.websocket = env.WebSocket
    env.crypt = {base64={encode=env.base64encode, decode=env.base64decode},
        base64encode=env.base64encode, base64decode=env.base64decode,
        base64_encode=env.base64encode, base64_decode=env.base64decode}
    env.crypt.hash = function(data, algorithm)
        assert(type(data) == "string", "crypt.hash expects a string")
        assert(algorithm == nil or type(algorithm) == "string", "hash algorithm must be a string")
        return env.request({BridgeOperation="crypto", Operation="hash", Data=env.base64encode(data),
            Algorithm=algorithm or "sha384"}).digest
    end
    env.crypt.generatebytes = function(count)
        assert(type(count) == "number" and count == math.floor(count) and count >= 0 and count <= 1024 * 1024,
            "byte count must be an integer from 0 to 1048576")
        return env.request({BridgeOperation="crypto", Operation="random", Count=count}).data
    end
    env.crypt.generatekey = function() return env.crypt.generatebytes(32) end
    env.crypt.random = function(count) return env.base64decode(env.crypt.generatebytes(count)) end
    local function aes(operation, data, key, iv, mode)
        assert(type(data) == "string" and type(key) == "string", "expected data and a Base64 key")
        assert(iv == nil or type(iv) == "string", "IV must be a Base64 string")
        assert(mode == nil or type(mode) == "string", "AES mode must be a string")
        assert(operation ~= "decrypt" or iv ~= nil or mode == "ECB", "decrypt requires an IV")
        local result = env.request({BridgeOperation="crypto", Operation=operation,
            Data=operation == "encrypt" and env.base64encode(data) or data, Key=key, IV=iv, Mode=mode or "CBC"})
        return result.data, result.iv
    end
    env.crypt.encrypt = function(data, key, iv, mode) return aes("encrypt", data, key, iv, mode) end
    env.crypt.decrypt = function(data, key, iv, mode)
        local plain = aes("decrypt", data, key, iv, mode)
        return env.base64decode(plain)
    end
    env.crypt.hmac = function(key, data, algorithm)
        assert(type(key) == "string" and type(data) == "string", "expected a raw key and data")
        assert(algorithm == nil or type(algorithm) == "string", "HMAC algorithm must be a string")
        return env.request({BridgeOperation="crypto", Operation="hmac", Key=env.base64encode(key),
            Data=env.base64encode(data), Algorithm=algorithm or "sha256"}).data
    end
    env.syn.crypt = env.crypt
    env.crypto = env.crypt

    env.Regex = {}
    env.Regex.Escape = function(text)
        assert(type(text) == "string", "expected text")
        return (text:gsub("([\\%.%^%$%|%?%*%+%(%)%[%]{}])", "\\%1"))
    end
    env.Regex.new = function(pattern)
        assert(type(pattern) == "string", "expected a regular expression")
        local function execute(operation, input, replacement)
            assert(input == nil or type(input) == "string", "expected input text")
            return env.request({BridgeOperation="regex", Operation=operation, Pattern=pattern,
                Input=input or "", Replacement=replacement})
        end
        execute("compile")
        return {
            Match=function(_, input) return execute("match", input).value end,
            MatchMany=function(_, input) return execute("many", input).values end,
            Replace=function(_, input, replacement)
                assert(type(replacement) == "string", "expected replacement text")
                return execute("replace", input, replacement).value
            end,
        }
    end
    env.getobjects = function(asset)
        assert(type(asset) == "string", "expected an asset URI")
        return game:GetObjects(asset)
    end

    local function recover(script, operation)
        assert(typeof(script) == "Instance" and script:IsA("LuaSourceContainer"), "expected a script Instance")
        local bytecode = env._getScriptBytecodeRaw(script)
        assert(bytecode and #bytecode > 0, "script has no client bytecode")
        assert(#bytecode <= 4 * 1024 * 1024, "decompiler bytecode exceeds 4 MiB")
        -- LunaUX publishes desktop binaries; Android uses the same provider's
        -- documented HTTPS API. Only an explicit decompile/disassemble sends bytes.
        local result = env.request({Url="https://lunaux-decompiler.dev/api/" .. operation,
            Method="POST", Timeout=60, FollowRedirects=false,
            Headers={["Content-Type"]="application/json"},
            Body=http:JSONEncode({bytecode=env.base64encode(bytecode)})})
        assert(result.Success, "LunaUX " .. operation .. " failed: HTTP " .. tostring(result.StatusCode))
        assert(type(result.Body) == "string" and result.Body:match("%S"), "LunaUX returned empty output")
        if operation == "decompile" then
            local compiled, problem = env.loadstring(result.Body, "=decompile-validation")
            assert(compiled, "LunaUX returned invalid Luau: " .. tostring(problem))
        end
        return result.Body
    end
    env.decompile = function(script) return recover(script, "decompile") end
    env.disassemble = function(script) return recover(script, "disassemble") end
end
