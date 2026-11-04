-- Local UI components. WindUI's dark theme and interaction styling informed
-- this adaptation; no remote library or image assets are needed at runtime.
return function()
    local UI = {}
    local TweenService = game:GetService("TweenService")
    UI.colors = {
        background = Color3.fromRGB(16, 16, 18),
        surface = Color3.fromRGB(42, 42, 45),
        inset = Color3.fromRGB(23, 23, 26),
        panel = Color3.fromRGB(28, 28, 31),
        outline = Color3.fromRGB(75, 75, 82),
        accent = Color3.fromRGB(0, 145, 255),
        accentDark = Color3.fromRGB(30, 48, 66),
        selected = Color3.fromRGB(34, 34, 38),
        text = Color3.fromRGB(248, 248, 251),
        muted = Color3.fromRGB(161, 161, 171),
        good = Color3.fromRGB(51, 199, 89),
        bad = Color3.fromRGB(255, 122, 137),
    }
    function UI.new(kind, name, parent, properties)
        local object = Instance.new(kind)
        object.Name = name
        for key, value in pairs(properties or {}) do object[key] = value end
        object.Parent = parent
        return object
    end
    function UI.round(object, radius)
        local corner = object:FindFirstChildOfClass("UICorner") or UI.new("UICorner", "Corner", object)
        corner.CornerRadius = UDim.new(0, radius)
        return object
    end
    function UI.outline(object, transparency)
        UI.new("UIStroke", "Stroke", object, {
            Color = UI.colors.outline, Transparency = transparency or 0.45, Thickness = 1,
        })
        return object
    end
    function UI.label(name, parent, value, size, position, color, fontSize)
        return UI.new("TextLabel", name, parent, {
            BackgroundTransparency = 1, Text = value, TextColor3 = color or UI.colors.text,
            TextSize = fontSize or 13, Font = Enum.Font.BuilderSans,
            Size = size, Position = position, TextXAlignment = Enum.TextXAlignment.Left,
            TextYAlignment = Enum.TextYAlignment.Center,
        })
    end
    function UI.button(name, parent, caption, size, position, fill, foreground)
        local control = UI.round(UI.new("TextButton", name, parent, {
            AutoButtonColor = false, BackgroundColor3 = fill or UI.colors.surface,
            BorderSizePixel = 0, Text = caption, TextColor3 = foreground or UI.colors.text,
            TextSize = 14, Font = Enum.Font.BuilderSansMedium, Size = size, Position = position,
        }), 10)
        local hover = UI.round(UI.new("Frame", "Hover", control, {
            BackgroundColor3 = Color3.new(1, 1, 1), BackgroundTransparency = 1,
            BorderSizePixel = 0, Size = UDim2.fromScale(1, 1), Active = false,
        }), 10)
        local currentTween
        local function shade(alpha)
            if currentTween then currentTween:Cancel() end
            currentTween = TweenService:Create(hover, TweenInfo.new(0.12), {BackgroundTransparency = alpha})
            currentTween:Play()
        end
        control.MouseEnter:Connect(function() shade(0.95) end)
        control.MouseLeave:Connect(function() shade(1) end)
        control.InputBegan:Connect(function(input)
            if input.UserInputType == Enum.UserInputType.Touch or input.UserInputType == Enum.UserInputType.MouseButton1 then shade(0.87) end
        end)
        control.InputEnded:Connect(function(input)
            if input.UserInputType == Enum.UserInputType.Touch or input.UserInputType == Enum.UserInputType.MouseButton1 then shade(1) end
        end)
        return control
    end
    function UI.icon(parent, kind, size, position, color)
        local frame = UI.new("Frame", "Icon", parent, {
            BackgroundTransparency = 1, Size = UDim2.fromOffset(size, size), Position = position,
        })
        local paths = {
            code = {{{8, 5}, {2, 12}, {8, 19}}, {{16, 5}, {22, 12}, {16, 19}}, {{14, 3}, {10, 21}}},
            folder = {{{3, 7}, {3, 4}, {9, 4}, {12, 7}, {21, 7}, {21, 20}, {3, 20}, {3, 7}}, {{3, 10}, {21, 10}}},
            terminal = {{{4, 6}, {9, 11}, {4, 16}}, {{12, 18}, {20, 18}}},
            settings = {{{5, 3}, {5, 7}}, {{5, 13}, {5, 21}}, {{12, 3}, {12, 14}}, {{12, 20}, {12, 21}}, {{19, 3}, {19, 5}}, {{19, 11}, {19, 21}}, {{2, 10}, {8, 10}}, {{9, 17}, {15, 17}}, {{16, 8}, {22, 8}}},
            play = {{{7, 4}, {20, 12}, {7, 20}, {7, 4}}},
            save = {{{5, 3}, {17, 3}, {21, 7}, {21, 21}, {3, 21}, {3, 3}, {5, 3}}, {{7, 3}, {7, 9}, {17, 9}, {17, 3}}, {{7, 21}, {7, 15}, {17, 15}, {17, 21}}},
        }
        for _, path in ipairs(paths[kind] or paths.code) do
            for i = 2, #path do
                local a = Vector2.new(path[i - 1][1], path[i - 1][2]) * (size / 24)
                local b = Vector2.new(path[i][1], path[i][2]) * (size / 24)
                local delta = b - a
                UI.round(UI.new("Frame", "Stroke", frame, {
                    BackgroundColor3 = color or UI.colors.muted, BorderSizePixel = 0,
                    AnchorPoint = Vector2.new(0.5, 0.5), Position = UDim2.fromOffset((a.X + b.X) / 2, (a.Y + b.Y) / 2),
                    Size = UDim2.fromOffset(delta.Magnitude, 1.6), Rotation = math.deg(math.atan2(delta.Y, delta.X)),
                }), 1)
            end
        end
        return frame
    end
    function UI.tintIcon(frame, color)
        for _, object in frame:GetChildren() do
            if object:IsA("Frame") then object.BackgroundColor3 = color end
        end
    end
    return UI
end
