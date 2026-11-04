return function(env)
    local event = Instance.new("BindableEvent")
    local current
    local rnet = {Capture = event.Event}

    function rnet.startcapture()
        local generation = env._rnetStart()
        current = generation
        task.spawn(function()
            local window, printed = time(), 0
            while current == generation do
                local packet = env._rnetPoll(generation)
                if packet == false then break end
                if packet then
                    local bytes = table.create(#packet)
                    for i = 1, #packet do bytes[i] = string.byte(packet, i) end
                    event:Fire(bytes)
                    local preview = table.create(math.min(#bytes, 64))
                    for i = 1, math.min(#bytes, 64) do
                        preview[i] = string.format("%02X", bytes[i])
                    end
                    if time() - window >= 1 then window, printed = time(), 0 end
                    if printed < 16 then
                        printed += 1
                        env.warn("rnet outgoing: " .. table.concat(preview, " ") .. (#bytes > 64 and " ..." or ""))
                    end
                else
                    task.wait(0.05)
                end
            end
        end)
    end

    function rnet.stopcapture()
        if current then env._rnetStop(current); current = nil end
    end

    local function byte(value)
        assert(type(value) == "number" and value >= 0 and value <= 255 and value % 1 == 0,
            "expected a byte (0..255)")
        return value
    end

    function rnet.setfilter(values)
        assert(type(values) == "table" and #values <= 64, "expected up to 64 prefix bytes")
        local chunks = table.create(#values)
        for i = 1, #values do chunks[i] = string.char(byte(values[i])) end
        env._rnetFilter(table.concat(chunks))
    end

    function rnet.sendraw(value)
        local payload
        if type(value) == "table" then
            assert(#value <= 65507, "packet too large")
            local chunks = table.create(#value)
            for i = 1, #value do chunks[i] = string.char(byte(value[i])) end
            payload = table.concat(chunks)
        else
            assert(type(value) == "string", "expected a hex string or byte table")
            local hex = value:gsub("%s", "")
            assert(#hex % 2 == 0 and hex:match("^[%x]*$"), "invalid hex packet")
            payload = hex:gsub("%x%x", function(pair) return string.char(tonumber(pair, 16)) end)
        end
        env._rnetSend(payload)
    end

    env.rnet = rnet
end
