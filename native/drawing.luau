return function(env, shared)
    local state = shared.__basalt_drawing
    if not state then
        state = {objects = {}, nextImage = 0}
        shared.__basalt_drawing = state
    end

    local function root()
        if state.root and state.root.Parent then return state.root end
        local gui = Instance.new("ScreenGui")
        gui.Name = "Basalt_Drawings"
        gui.ResetOnSpawn = false
        gui.IgnoreGuiInset = true
        gui.DisplayOrder = 1000
        gui.Parent = env.gethui()
        state.root = gui
        return gui
    end

    local shapeFields = {
        Line = {From="Vector2", To="Vector2", Thickness="number"},
        Text = {Text="string", Font="number", Size="number", Position="Vector2", Center="boolean", Outline="boolean", OutlineColor="Color3"},
        Image = {Data="string", Size="Vector2", Position="Vector2", Rounding="number"},
        Circle = {NumSides="number", Radius="number", Position="Vector2", Thickness="number", Filled="boolean"},
        Square = {Size="Vector2", Position="Vector2", Thickness="number", Filled="boolean"},
        Quad = {PointA="Vector2", PointB="Vector2", PointC="Vector2", PointD="Vector2", Thickness="number", Filled="boolean"},
        Triangle = {PointA="Vector2", PointB="Vector2", PointC="Vector2", Thickness="number", Filled="boolean"},
    }
    local sharedFields = {Visible="boolean", ZIndex="number", Transparency="number", Color="Color3"}
    local fonts = {[0] = Enum.Font.SourceSans, [1] = Enum.Font.Arial, [2] = Enum.Font.Code, [3] = Enum.Font.RobotoMono}
    local function segment(frame, a, b, thickness, color, transparency, z)
        local delta = b - a
        frame.AnchorPoint = Vector2.new(0.5, 0.5)
        frame.Position = UDim2.fromOffset((a.X + b.X) / 2, (a.Y + b.Y) / 2)
        frame.Size = UDim2.fromOffset(delta.Magnitude, thickness)
        frame.Rotation = math.deg(math.atan2(delta.Y, delta.X))
        frame.BackgroundColor3 = color
        frame.BackgroundTransparency = transparency
        frame.ZIndex = z
    end
    local function redraw(record)
        local props, visuals, kind = record.properties, record.visuals, record.kind
        if not record.alive then return end
        for _, visual in visuals do visual:Destroy() end
        table.clear(visuals)
        if not props.Visible then return end
        local parent, color, alpha, z = root(), props.Color, 1 - props.Transparency, props.ZIndex
        local function frame()
            local item = Instance.new("Frame")
            item.Name = "Drawing" .. kind
            item.BorderSizePixel = 0
            item.Parent = parent
            visuals[#visuals + 1] = item
            return item
        end
        local function line(a, b)
            segment(frame(), a, b, props.Thickness, color, alpha, z)
        end
        if kind == "Line" then
            line(props.From, props.To)
        elseif kind == "Text" then
            local item = Instance.new("TextLabel")
            item.Name = "DrawingText"
            item.BackgroundTransparency = 1
            item.Text = props.Text
            item.TextColor3 = color
            item.TextTransparency = alpha
            item.TextStrokeColor3 = props.OutlineColor
            item.TextStrokeTransparency = props.Outline and alpha or 1
            item.Font = fonts[props.Font] or fonts[0]
            item.TextSize = props.Size
            item.TextXAlignment = props.Center and Enum.TextXAlignment.Center or Enum.TextXAlignment.Left
            local bounds = game:GetService("TextService"):GetTextSize(props.Text, props.Size, item.Font, Vector2.new(100000, 100000))
            item.Size = UDim2.fromOffset(math.max(1, bounds.X + 2), math.max(1, bounds.Y + 2))
            item.Position = UDim2.fromOffset(props.Position.X - (props.Center and bounds.X / 2 or 0), props.Position.Y)
            item.ZIndex = z
            item.Parent = parent
            visuals[#visuals + 1] = item
        elseif kind == "Image" then
            local item = Instance.new("ImageLabel")
            item.Name = "DrawingImage"
            item.BackgroundTransparency = 1
            item.Size = UDim2.fromOffset(props.Size.X, props.Size.Y)
            item.Position = UDim2.fromOffset(props.Position.X, props.Position.Y)
            item.ImageTransparency = alpha
            item.ImageColor3 = color
            item.ZIndex = z
            if #props.Data > 0 then
                state.nextImage += 1
                local path = "basalt-drawings/" .. tostring(state.nextImage) .. ".png"
                env.makefolder("basalt-drawings")
                env.writefile(path, props.Data)
                item.Image = env.getcustomasset(path)
                env.delfile(path)
            end
            item.Parent = parent
            visuals[#visuals + 1] = item
            if props.Rounding > 0 then
                local corner = Instance.new("UICorner")
                corner.CornerRadius = UDim.new(0, props.Rounding)
                corner.Parent = item
            end
        elseif kind == "Circle" or kind == "Square" then
            local item = frame()
            local size = kind == "Circle" and Vector2.new(props.Radius * 2, props.Radius * 2) or props.Size
            local start = kind == "Circle" and props.Position - size / 2 or props.Position
            item.Position = UDim2.fromOffset(start.X, start.Y)
            item.Size = UDim2.fromOffset(size.X, size.Y)
            item.BackgroundColor3 = color
            item.BackgroundTransparency = props.Filled and alpha or 1
            item.ZIndex = z
            if kind == "Circle" then
                local corner = Instance.new("UICorner")
                corner.CornerRadius = UDim.new(1, 0)
                corner.Parent = item
            end
            if not props.Filled then
                local stroke = Instance.new("UIStroke")
                stroke.Color = color
                stroke.Thickness = props.Thickness
                stroke.Transparency = alpha
                stroke.Parent = item
            end
        else
            local points = {props.PointA, props.PointB, props.PointC}
            if kind == "Quad" then points[4] = props.PointD end
            for i = 1, #points do line(points[i], points[i % #points + 1]) end
        end
    end

    local function defaults(kind)
        local props = {Visible=false, ZIndex=0, Transparency=1, Color=Color3.new(1, 1, 1),
            Thickness=1, Filled=false, Position=Vector2.zero, Size=kind == "Text" and 16 or Vector2.zero,
            Radius=0, NumSides=32, From=Vector2.zero, To=Vector2.zero,
            PointA=Vector2.zero, PointB=Vector2.zero, PointC=Vector2.zero, PointD=Vector2.zero,
            Text="", Font=0, Center=false, Outline=false, OutlineColor=Color3.new(), Data="", Rounding=0}
        return props
    end
    local function alive(drawing)
        local record = state.objects[drawing]
        assert(record and record.alive, "invalid Drawing object")
        return record
    end
    local fontNames = {UI=0, System=1, Plex=2, Monospace=3}
    env.Drawing = {Fonts = fontNames, Font = fontNames}
    env.Drawing.new = function(kind)
        assert(type(kind) == "string" and shapeFields[kind], "unknown Drawing type")
        local proxy = newproxy(true)
        local record = {alive=true, kind=kind, properties=defaults(kind), visuals={}}
        state.objects[proxy] = record
        local mt = getmetatable(proxy)
        mt.__index = function(self, key)
            if key == "__OBJECT_EXISTS" then return record.alive end
            if key == "Remove" or key == "Destroy" then
                return function(target)
                    assert(target == proxy, "expected a Drawing object")
                    if not record.alive then return end
                    for _, visual in record.visuals do visual:Destroy() end
                    table.clear(record.visuals)
                    record.alive = false
                    state.objects[proxy] = nil
                end
            end
            local props = record.properties
            if key == "TextBounds" and kind == "Text" then
                return game:GetService("TextService"):GetTextSize(props.Text, props.Size, fonts[props.Font] or fonts[0], Vector2.new(100000, 100000))
            end
            assert(sharedFields[key] or shapeFields[kind][key], "unknown Drawing property")
            return props[key]
        end
        mt.__newindex = function(self, key, value)
            alive(self)
            local expected = sharedFields[key] or shapeFields[kind][key]
            assert(expected, "unknown or readonly Drawing property")
            assert(typeof(value) == expected, "Drawing property type mismatch")
            if expected == "number" then
                assert(value == value and math.abs(value) < 1e6, "invalid Drawing number")
                if key == "Transparency" then assert(value >= 0 and value <= 1, "invalid transparency") end
                if key == "Font" then assert(fonts[value], "unknown Drawing font") end
            end
            record.properties[key] = value
            redraw(record)
        end
        return proxy
    end
    env.isrenderobj = function(value)
        local record = state.objects[value]
        return record ~= nil and record.alive
    end
    env.getrenderproperty = function(drawing, property)
        alive(drawing)
        return drawing[property]
    end
    env.setrenderproperty = function(drawing, property, value)
        alive(drawing)
        drawing[property] = value
    end
    env.cleardrawcache = function()
        for drawing, _ in state.objects do drawing:Destroy() end
    end
end
