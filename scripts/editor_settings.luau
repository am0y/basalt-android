-- Bundled as createEditorSettings(context); preferences are local to this installation.
local new, rounded, label, button, setUnit = context.new, context.rounded, context.label, context.button, context.setUnit
local colors, observe = context.colors, context.observe
local defaults = {textSize=13, syntaxHighlighting=true, lineNumbers=true, autoIndent=true,
    backgroundBlur=true, clearConsoleOnExecute=false, vulnerabilityProtection=true,
    maxFps=60, uiScale=100, antiAfk=false, autoexecute=true, executorName="Basalt"}
local values = table.clone(defaults)
local path = ".basalt/settings.json"
local function valid(name, value)
    if name == "textSize" then return type(value) == "number" and value >= 11 and value <= 17 and value % 1 == 0 end
    if name == "maxFps" then return type(value) == "number" and value >= 1 and value <= 240 and value % 1 == 0 end
    if name == "uiScale" then return type(value) == "number" and value >= 80 and value <= 120 and value % 1 == 0 end
    if name == "executorName" then return type(value) == "string" and #value > 0 and #value <= 32 and not value:find("[^%g ]") end
    return defaults[name] ~= nil and type(value) == "boolean"
end
pcall(function()
    local encoded = readfile(path)
    if type(encoded) ~= "string" or #encoded > 4096 then return end
    local saved = context.http:JSONDecode(encoded)
    if type(saved) ~= "table" or saved.version ~= 1 or type(saved.preferences) ~= "table" then return end
    for name in defaults do
        if valid(name, saved.preferences[name]) then values[name] = saved.preferences[name] end
    end
end)
context.shared.__basalt_executor_name=values.executorName
local function applyProtection(value)
    if type(context.protection) ~= "function" then return false end
    local ok, actual = pcall(context.protection, value)
    return ok and actual == value
end
if not applyProtection(values.vulnerabilityProtection) then values.vulnerabilityProtection = true end

local card = rounded(new("Frame", "SettingsCard", context.panel, {
    BackgroundColor3=colors.panel, BackgroundTransparency=0.08,
    Position=UDim2.fromOffset(0, 77), Size=UDim2.fromOffset(536, 233),
    ClipsDescendants=true, Visible=false, ZIndex=5,
}), 17)
local list = new("ScrollingFrame", "Preferences", card, {
    BackgroundTransparency=1, Position=UDim2.fromOffset(14, 12), Size=UDim2.new(1, -28, 1, -24),
    CanvasSize=UDim2.fromOffset(0, 0), CanvasPosition=Vector2.new(0, 0),
    AutomaticCanvasSize=Enum.AutomaticSize.None, Active=true, ScrollingEnabled=true,
    ScrollingDirection=Enum.ScrollingDirection.Y, ScrollBarThickness=2,
    ScrollBarImageColor3=colors.muted, ScrollBarImageTransparency=0.55,
    VerticalScrollBarInset=Enum.ScrollBarInset.Always, ZIndex=6,
})
local rows = {}
local cursor=6
local function category(name)
    local heading=label("Category"..name,list,name,UDim2.new(1,-12,0,20),
        UDim2.fromOffset(0,cursor),11,colors.muted)
    heading.Font=Enum.Font.BuilderSansBold
    cursor+=27
end
local function row(name, caption, hint)
    local frame = new("Frame", name, list, {
        BackgroundTransparency=1, Size=UDim2.new(1, -4, 0, 46), Position=UDim2.fromOffset(0, cursor), ZIndex=7,
    })
    cursor+=52
    local heading = label("Title", frame, caption, UDim2.new(1, -120, 0, 17), UDim2.fromOffset(0, 6), 11)
    heading.Font = Enum.Font.BuilderSansBold
    label("Description", frame, hint, UDim2.new(1, -120, 0, 15), UDim2.fromOffset(0, 24), 9, colors.muted)
    rows[name] = frame
    return frame
end
category("Performance")
local fpsRow=row("MaxFps","Max FPS","1-240, adjusts by 5 FPS")
local sizeRow=row("UiScale","UI size","80-120%, adjusts by 5%")
local function stepper(frame,initial)
    local decrease=button("Decrease",frame,"",UDim2.fromOffset(28,28),UDim2.new(1,-100,0.5,-14),colors.panel)
    local increase=button("Increase",frame,"",UDim2.fromOffset(28,28),UDim2.new(1,-40,0.5,-14),colors.panel)
    for _,control in {decrease,increase} do setUnit(control.Corner,"CornerRadius",UDim.new(0.5,0)) end
    local minus=new("Frame","Icon",decrease,{BackgroundColor3=colors.text,Size=UDim2.fromOffset(10,1.2),
        Position=UDim2.fromScale(0.5,0.5),AnchorPoint=Vector2.new(0.5,0.5),ZIndex=decrease.ZIndex+1})
    rounded(minus,1)
    local plus=context.icon(increase,"add",14,UDim2.fromScale(0.5,0.5))
    plus.AnchorPoint=Vector2.new(0.5,0.5)
    local value=label("Value",frame,initial,UDim2.fromOffset(32,28),UDim2.new(1,-72,0.5,-14),11)
    value.TextXAlignment=Enum.TextXAlignment.Center
    value.Font=Enum.Font.BuilderSansBold
    return {decrease=decrease,increase=increase,value=value,minus=minus,plus=plus}
end
local fpsControl=stepper(fpsRow,tostring(values.maxFps))
local uiSizeControl=stepper(sizeRow,tostring(values.uiScale).."%")
local toggles = {}
local function toggle(name, preference, caption, hint)
    local frame = row(name, caption, hint)
    local control = button("Toggle", frame, "", UDim2.fromOffset(62, 28), UDim2.new(1, -74, 0.5, -14), colors.accent, colors.ink)
    setUnit(control.Corner, "CornerRadius", UDim.new(0.5, 0))
    toggles[preference] = control
    return control
end
toggle("BackgroundBlur", "backgroundBlur", "Background blur", "Soften the game behind the menu")
category("Automation")
toggle("AntiAfk", "antiAfk", "Anti-AFK", "Prevent idle disconnects when available")
toggle("Autoexecute", "autoexecute", "Autoexecute", "Run .lua and .luau from basalt/autoexec")
category("Compatibility")
local nameRow=row("ExecutorName","Executor name","Reported by identifyexecutor()")
local nameInput=rounded(new("TextBox","Name",nameRow,{
    Text=values.executorName,PlaceholderText="Basalt",ClearTextOnFocus=false,
    BackgroundColor3=colors.selected,TextColor3=colors.text,PlaceholderColor3=colors.muted,
    TextXAlignment=Enum.TextXAlignment.Center,Font=Enum.Font.BuilderSansMedium,TextSize=10,
    Size=UDim2.fromOffset(108,28),Position=UDim2.new(1,-120,0.5,-14),ZIndex=8,
}),14)
category("Editor")
local textRow = row("TextSize", "Text size", "Adjust code readability")
local smaller = button("Decrease", textRow, "", UDim2.fromOffset(28, 28), UDim2.new(1, -100, 0.5, -14), colors.panel)
local larger = button("Increase", textRow, "", UDim2.fromOffset(28, 28), UDim2.new(1, -40, 0.5, -14), colors.panel)
for _, control in {smaller, larger} do setUnit(control.Corner, "CornerRadius", UDim.new(0.5, 0)) end
local minus = new("Frame", "Icon", smaller, {BackgroundColor3=colors.text, Size=UDim2.fromOffset(10, 1.2),
    Position=UDim2.fromScale(0.5, 0.5), AnchorPoint=Vector2.new(0.5, 0.5), ZIndex=smaller.ZIndex+1})
rounded(minus, 1)
local plus = context.icon(larger, "add", 14, UDim2.fromScale(0.5, 0.5))
plus.AnchorPoint = Vector2.new(0.5, 0.5)
local sizeValue = label("Value", textRow, "", UDim2.fromOffset(32, 28), UDim2.new(1, -72, 0.5, -14), 11)
sizeValue.TextXAlignment = Enum.TextXAlignment.Center
sizeValue.Font = Enum.Font.BuilderSansBold
toggle("SyntaxHighlighting", "syntaxHighlighting", "Syntax highlighting", "Colorize code as you edit")
toggle("LineNumbers", "lineNumbers", "Line numbers", "Show numbers beside your code")
toggle("AutoIndent", "autoIndent", "Auto-indent", "Indent new lines automatically")
toggle("ClearConsoleOnExecute", "clearConsoleOnExecute", "Clear console on Execute", "Start each run with fresh output")
if type(context.protection)=="function" then
    category("Protection")
    toggle("VulnerabilityProtection", "vulnerabilityProtection", "Vulnerability protection", "Block restricted service calls")
end
local settings = {card=card, controls={decrease=smaller, increase=larger, syntax=toggles.syntaxHighlighting,
    blur=toggles.backgroundBlur, protection=toggles.vulnerabilityProtection, lineNumbers=toggles.lineNumbers,
    autoIndent=toggles.autoIndent, clearConsole=toggles.clearConsoleOnExecute,
    maxFpsDecrease=fpsControl.decrease,maxFpsIncrease=fpsControl.increase,maxFpsValue=fpsControl.value,
    uiScaleDecrease=uiSizeControl.decrease,uiScaleIncrease=uiSizeControl.increase,uiScaleValue=uiSizeControl.value,
    antiAfk=toggles.antiAfk, autoexecute=toggles.autoexecute,
    executorName=nameInput}}
local function refresh()
    sizeValue.Text = tostring(values.textSize)
    fpsControl.value.Text=tostring(values.maxFps)
    uiSizeControl.value.Text=tostring(values.uiScale).."%"
    if not nameInput:IsFocused() then nameInput.Text=values.executorName end
    for _,spec in {{fpsControl,values.maxFps,1,240},{uiSizeControl,values.uiScale,80,120}} do
        local control,value,low,high=spec[1],spec[2],spec[3],spec[4]
        control.decrease.Active,control.increase.Active=value>low,value<high
        control.minus.BackgroundTransparency=control.decrease.Active and 0 or 0.65
        control.plus.ImageTransparency=control.increase.Active and 0 or 0.65
    end
    smaller.Active, larger.Active = values.textSize > 11, values.textSize < 17
    minus.BackgroundTransparency, plus.ImageTransparency = smaller.Active and 0 or 0.65, larger.Active and 0 or 0.65
    for name, control in toggles do
        control.Text = values[name] and "On" or "Off"
        control.BackgroundColor3 = values[name] and colors.accent or colors.panel
        control.TextColor3 = values[name] and colors.ink or colors.muted
    end
    if rows.VulnerabilityProtection then
        rows.VulnerabilityProtection.Description.Text = values.vulnerabilityProtection and "Block restricted service calls" or "Allow restricted service calls"
    end
end
local function save()
    local ok = pcall(function()
        writefile(path, context.http:JSONEncode({version=1, preferences=values}))
    end)
    if not ok and context.reportError then context.reportError("Settings: couldn't save preferences") end
end
local idleConnection
local function applyAntiAfk(enabled)
    if idleConnection then idleConnection:Disconnect(); idleConnection=nil end
    if not enabled then return true end
    local ok, virtual=pcall(function() return game:GetService("VirtualUser") end)
    if not ok then return false end
    local connected, connection=pcall(function()
        return game:GetService("Players").LocalPlayer.Idled:Connect(function()
            local worked=pcall(function()
                virtual:CaptureController()
                virtual:ClickButton2(Vector2.new(0,0))
            end)
            if not worked then
                if idleConnection then idleConnection:Disconnect(); idleConnection=nil end
                values.antiAfk=false
                refresh()
                save()
            end
        end)
    end)
    if not connected then return false end
    idleConnection=connection
    return true
end
function settings.get() return table.clone(values) end
function settings.set(name, value)
    if not valid(name, value) then return false end
    if values[name] == value then return true end
    if name == "vulnerabilityProtection" and not applyProtection(value) then
        if context.reportError then context.reportError("Settings: native protection control is unavailable") end
        return false
    end
    if name == "maxFps" then
        local ok,problem=pcall(setfpscap,value)
        if not ok then
            if context.reportError then context.reportError("Settings: FPS limit: "..tostring(problem)) end
            return false
        end
    end
    if name == "antiAfk" and not applyAntiAfk(value) then
        if context.reportError then context.reportError("Settings: Anti-AFK unavailable") end
        return false
    end
    local before = table.clone(values)
    values[name] = value
    if name == "executorName" then context.shared.__basalt_executor_name=value end
    if settings.changed then settings.changed(table.clone(values), before) end
    refresh()
    save()
    return true
end
function settings.destroy()
    if idleConnection then idleConnection:Disconnect(); idleConnection=nil end
end
function settings.layout(width, height, header)
    setUnit(card, "Position", UDim2.fromOffset(0, header))
    setUnit(card, "Size", UDim2.fromOffset(width, height - header))
    local bottom=0
    for _,frame in rows do bottom=math.max(bottom,frame.Position.Y.Offset+frame.Size.Y.Offset) end
    -- Use the resized row bounds; cached automatic bounds can outlive a scale change.
    setUnit(list,"CanvasSize",UDim2.fromOffset(0,math.ceil(bottom/context.scale())+14))
end
observe(smaller.Activated, function() settings.set("textSize", values.textSize - 1) end)
observe(larger.Activated, function() settings.set("textSize", values.textSize + 1) end)
observe(fpsControl.decrease.Activated,function() settings.set("maxFps",math.max(1,values.maxFps-5)) end)
observe(fpsControl.increase.Activated,function() settings.set("maxFps",math.min(240,values.maxFps+5)) end)
observe(uiSizeControl.decrease.Activated,function() settings.set("uiScale",math.max(80,values.uiScale-5)) end)
observe(uiSizeControl.increase.Activated,function() settings.set("uiScale",math.min(120,values.uiScale+5)) end)
observe(nameInput.FocusLost,function()
    local name=nameInput.Text:match("^%s*(.-)%s*$")
    if not settings.set("executorName",name) then nameInput.Text=values.executorName end
end)
for name, control in toggles do observe(control.Activated, function() settings.set(name, not values[name]) end) end
for _,control in settings.controls do
    if control:IsA("GuiButton") then context.buttonMotion(control) end
end
pcall(function()
    if not isfolder("basalt") then makefolder("basalt") end
    if not isfolder("basalt/autoexec") then makefolder("basalt/autoexec") end
end)
pcall(setfpscap,values.maxFps)
if values.antiAfk and not applyAntiAfk(true) then values.antiAfk=false;save() end
refresh()
return settings
