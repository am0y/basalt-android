return function(env, sharedEnvironment, compile, entry, diagnostics, actorRuntime)
    local bus = sharedEnvironment.__basaltConsole
    if not bus then
        bus = {history={}, listeners={}, sources={}, serial=0}
        function bus.publish(text, kind)
            local record = {message=text, messageType=kind}
            bus.history[#bus.history + 1] = record
            if #bus.history > 300 then table.remove(bus.history, 1) end
            for callback in bus.listeners do task.defer(callback, text, kind) end
        end
        function bus.subscribe(callback)
            bus.listeners[callback] = true
            return {Disconnect=function() bus.listeners[callback] = nil end}
        end
        -- Error supplies the actual VM traceback separately from the message.
        -- Match registered chunks, never a message prefix or a nil script alone.
        if not actorRuntime then
            bus.errors = game:GetService("ScriptContext").Error:Connect(function(message, stack, scriptObject)
                if scriptObject ~= nil then return end
                for line in stack:gmatch("[^\n]+") do
                    local source = line:match("^(.-), line %d+") or line:match("^(.-), function ") or line
                    if bus.sources[source] then
                        bus.publish(message .. (stack ~= "" and ("\n" .. stack) or ""), Enum.MessageType.MessageError)
                        return
                    end
                end
            end)
        end
        sharedEnvironment.__basaltConsole = bus
    end
    bus.sources[debug.info(entry, "s")] = true
    local roblox = getmetatable(sharedEnvironment).__index
    local function printer(native, kind)
        local proxy = env.clonefunction(native)
        env.hookfunction(proxy, function(...)
            local values = table.pack(...)
            -- Format once so side-effecting __tostring methods retain the same
            -- behavior in the game log and the editor console.
            for index = 1, values.n do values[index] = tostring(values[index]) end
            if diagnostics or actorRuntime then native(table.unpack(values, 1, values.n)) end
            bus.publish(table.concat(values, " ", 1, values.n), kind)
        end)
        return proxy
    end
    env.print = printer(roblox.print, Enum.MessageType.MessageOutput)
    env.warn = printer(roblox.warn, Enum.MessageType.MessageWarning)
    local function closeConsole()
        local window = bus.window
        if not window then return end
        bus.window = nil
        window.closed = true
        window.subscription:Disconnect()
        window.gui:Destroy()
        if window.waiting then window.input:Fire(nil) end
        task.defer(function() window.input:Destroy() end)
    end
    local function openConsole()
        if bus.window then return bus.window end
        local gui = Instance.new("ScreenGui")
        gui.Name, gui.ResetOnSpawn, gui.DisplayOrder = "BasaltConsole", false, 1000
        local panel = Instance.new("Frame")
        panel.Name, panel.Size, panel.Position = "Window", UDim2.fromScale(0.8, 0.65), UDim2.fromScale(0.1, 0.15)
        panel.BackgroundColor3 = Color3.fromRGB(20, 22, 27)
        panel.Parent = gui
        local title = Instance.new("TextLabel")
        title.Name, title.Text, title.Size = "Title", "Basalt Console", UDim2.new(1, -44, 0, 34)
        title.BackgroundTransparency, title.TextColor3, title.Font = 1, Color3.new(1, 1, 1), Enum.Font.Code
        title.TextSize, title.Parent = 16, panel
        local close = Instance.new("TextButton")
        close.Name, close.Text, close.Size, close.Position = "Close", "X", UDim2.fromOffset(34, 34), UDim2.new(1, -34, 0, 0)
        close.Parent = panel
        local log = Instance.new("ScrollingFrame")
        log.Name, log.Position, log.Size = "Output", UDim2.fromOffset(8, 38), UDim2.new(1, -16, 1, -82)
        log.BackgroundTransparency, log.BorderSizePixel, log.ScrollBarThickness = 1, 0, 5
        log.AutomaticCanvasSize, log.CanvasSize, log.Parent = Enum.AutomaticSize.Y, UDim2.new(), panel
        local layout = Instance.new("UIListLayout")
        layout.SortOrder, layout.Parent = Enum.SortOrder.LayoutOrder, log
        local input = Instance.new("TextBox")
        input.Name, input.PlaceholderText, input.Text = "Input", "Console input", ""
        input.Position, input.Size = UDim2.new(0, 8, 1, -36), UDim2.new(1, -16, 0, 28)
        input.Font, input.TextSize, input.TextColor3 = Enum.Font.Code, 14, Color3.new(1, 1, 1)
        input.BackgroundColor3, input.ClearTextOnFocus, input.Parent = Color3.fromRGB(35, 38, 45), false, panel
        local window = {gui=gui, title=title, log=log, box=input, input=Instance.new("BindableEvent"), lines={}, serial=0}
        local function append(text, kind)
            if window.closed then return end
            local line = Instance.new("TextLabel")
            window.serial += 1
            line.LayoutOrder, line.Text = window.serial, text
            line.Font, line.TextSize, line.TextWrapped = Enum.Font.Code, 14, true
            line.TextXAlignment, line.AutomaticSize = Enum.TextXAlignment.Left, Enum.AutomaticSize.Y
            line.Size, line.BackgroundTransparency = UDim2.new(1, -8, 0, 18), 1
            line.TextColor3 = kind == Enum.MessageType.MessageError and Color3.fromRGB(255, 110, 110) or
                kind == Enum.MessageType.MessageWarning and Color3.fromRGB(255, 205, 100) or Color3.fromRGB(225, 230, 240)
            line.Parent = log
            window.lines[#window.lines + 1] = line
            if #window.lines > 300 then table.remove(window.lines, 1):Destroy() end
        end
        window.subscription = bus.subscribe(append)
        for _, record in bus.history do append(record.message, record.messageType) end
        close.Activated:Connect(closeConsole)
        input.FocusLost:Connect(function(enterPressed)
            if enterPressed and window.waiting then window.input:Fire(input.Text) end
        end)
        local ok, problem = pcall(function() gui.Parent = env.gethui() end)
        if not ok then window.subscription:Disconnect(); gui:Destroy(); window.input:Destroy(); error(problem, 2) end
        bus.window = window
        return window
    end
    env.rconsolecreate = function() openConsole() end
    env.rconsoledestroy = closeConsole
    env.rconsolesettitle = function(title)
        assert(type(title) == "string", "expected a console title")
        openConsole().title.Text = title
    end
    env.rconsoleclear = function()
        table.clear(bus.history)
        if bus.window then
            for _, line in bus.window.lines do line:Destroy() end
            table.clear(bus.window.lines)
        end
    end
    env.rconsoleinput = function()
        local window = openConsole()
        assert(not window.waiting or coroutine.status(window.waiting) == "dead", "another console input is pending")
        window.waiting = coroutine.running()
        window.box.Text = ""
        window.box:CaptureFocus()
        local text = window.input.Event:Wait()
        window.waiting = false
        assert(text ~= nil, "console was closed while waiting for input")
        return text
    end
    env.rconsoleinfo = env.print
    env.loadstring = function(source, name)
        bus.serial += 1
        local compiled, problem = compile(source, (name or "=Basalt") .. " [" .. bus.serial .. "]")
        if compiled then bus.sources[debug.info(compiled, "s")] = true end
        return compiled, problem
    end
    return bus
end
