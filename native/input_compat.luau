return function(env)
    local camera = workspace.CurrentCamera
    local userInput = game:GetService("UserInputService")
    local virtual
    local ok, service = pcall(game.GetService, game, "VirtualInputManager")
    if ok then virtual = service end
    local function method(name)
        if not virtual then return nil end
        local available, implementation = pcall(function() return virtual[name] end)
        return available and implementation or nil
    end
    local sendKey, sendMouse = method("SendKeyEvent"), method("SendMouseButtonEvent")
    local desiredX, desiredY
    local function viewport()
        camera = workspace.CurrentCamera or camera
        assert(camera, "game camera is unavailable")
        return camera.ViewportSize
    end
    local function send(kind, code, x, y)
        local size = viewport()
        env._inputEvent(kind, code or 0, x or 0, y or 0, size.X, size.Y)
    end
    local function keycode(value)
        if typeof(value) == "EnumItem" then
            assert(value.EnumType == Enum.KeyCode, "expected an Enum.KeyCode")
            return value.Value
        end
        if type(value) == "string" then
            if #value == 1 then return string.byte(value) end
            local item = Enum.KeyCode[value]
            assert(item, "unknown key name")
            return item.Value
        end
        assert(type(value) == "number" and value == math.floor(value) and value >= 0,
            "expected a key name, Enum.KeyCode, or integer")
        return value
    end
    local function nativeKey(pressed, key)
        local code = keycode(key)
        if sendKey then
            local item = typeof(key) == "EnumItem" and key or type(key) == "string" and #key == 1 and
                Enum.KeyCode[key:upper()] or Enum.KeyCode:FromValue(code)
            env._inputNativeCall(sendKey, virtual, pressed, item, false, game)
        else send(pressed and 0 or 1, code) end
    end
    env.keypress = function(key) nativeKey(true, key) end
    env.keyrelease = function(key) nativeKey(false, key) end
    env.keytap = function(key)
        nativeKey(true, key)
        nativeKey(false, key)
    end
    env.mousemoveabs = function(x,y)
        desiredX, desiredY = x, y
        send(2, 0, x, y)
    end
    env.mousemoverel = function(dx,dy)
        local here = userInput:GetMouseLocation()
        desiredX, desiredY = (desiredX or here.X) + dx, (desiredY or here.Y) + dy
        send(3, 0, dx, dy)
    end
    local function mouse(button, pressed)
        if sendMouse then
            local here = userInput:GetMouseLocation()
            local code = button == 1 and 0 or button == 2 and 1 or 2
            env._inputNativeCall(sendMouse, virtual, math.floor(desiredX or here.X),
                math.floor(desiredY or here.Y), code, pressed, game, 0)
        else send(pressed and 4 or 5, button) end
    end
    env.mouse1press = function() mouse(1, true) end
    env.mouse1release = function() mouse(1, false) end
    env.mouse2press = function() mouse(2, true) end
    env.mouse2release = function() mouse(2, false) end
    env.mouse3press = function() mouse(4, true) end
    env.mouse3release = function() mouse(4, false) end
    env.mouse1click = function() mouse(1, true); mouse(1, false) end
    env.mouse2click = function() mouse(2, true); mouse(2, false) end
    env.mouse3click = function() mouse(4, true); mouse(4, false) end
    env.mousescroll = function(amount) send(6, 0, amount, 0) end
    env.touchpress = function(x,y,id) send(8, id or 0, x, y) end
    env.touchmove = function(x,y,id) send(9, id or 0, x, y) end
    env.touchrelease = function(x,y,id) send(10, id or 0, x, y) end
    env.touchclick = function(x,y,id) send(11, id or 0, x, y) end
end
