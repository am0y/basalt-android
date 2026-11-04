return function(env)
    local http = env.cloneref(game:GetService("HttpService"))
    local function call(options)
        options.BridgeOperation = "websocket"
        local id, problem = env._requestStart(http:JSONEncode(options))
        assert(id, problem)
        if options.Action ~= "connect" then
            -- Poll/send/close only touch the on-device socket queue. Waiting
            -- for their worker completion avoids adding whole render frames
            -- to every message, especially on a busy translated client.
            local ready, body, errorText = env._requestAwait(id)
            assert(ready and body, errorText)
            local result = http:JSONDecode(body)
            assert(not result.error, result.error)
            return result
        end
        while true do
            local ready, body, errorText = env._requestPoll(id)
            if ready then
                assert(body, errorText)
                local result = http:JSONDecode(body)
                assert(not result.error, result.error)
                return result
            end
            local ok, waitError = pcall(task.wait)
            if not ok then
                if tostring(waitError):find("thread is not yieldable", 1, true) then
                    local done, response, failure = env._requestAwait(id)
                    assert(done and response, failure or waitError)
                    local result = http:JSONDecode(response)
                    assert(not result.error, result.error)
                    return result
                end
                env._requestCancel(id)
                error(waitError, 2)
            end
        end
    end
    env.WebSocket = {}
    env.WebSocket.connect = function(url)
        assert(type(url) == "string", "WebSocket.connect expects a URL")
        local id = call({Action="connect", Url=url}).id
        local message, closed = Instance.new("BindableEvent"), Instance.new("BindableEvent")
        local state, closing = true, false
        local socket = {OnMessage=message.Event, OnClose=closed.Event}
        function socket:Send(text)
            assert(state and not closing, "WebSocket is closed")
            assert(type(text) == "string", "Send expects a string")
            call({Action="send", Id=id, Message=text})
        end
        function socket:Close()
            if not state or closing then return end
            closing = true
            call({Action="close", Id=id})
        end
        -- Return the socket before reading queued server messages so callers
        -- can attach OnMessage/OnClose listeners even when the host is ready.
        task.defer(function()
            while state do
                local ok, result = pcall(call, {Action="poll", Id=id})
                if ok then
                    for _, event in result.events do
                        message:Fire(event.message or env.base64decode(event.base64))
                    end
                end
                if not ok or result.closed then
                    state = false
                    closed:Fire()
                    task.defer(function() message:Destroy(); closed:Destroy() end)
                    return
                end
                -- Keep message/close delivery responsive; each host request
                -- already yields until its asynchronous result is available.
                task.wait(0.01)
            end
        end)
        return socket
    end
end
