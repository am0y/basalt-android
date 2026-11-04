-- Pixel-aligned editor: native input and rendered code share one coordinate space.
local VERSION = "basalt-android-core-1"
local MAX_SCRIPT_BYTES = 8 * 1024 * 1024
local DEFAULT_SCRIPT = '-- Write your Luau script here\nprint("Hello from Basalt")'
local Players = cloneref(game:GetService("Players"))
local GuiService = cloneref(game:GetService("GuiService"))
local UserInputService = cloneref(game:GetService("UserInputService"))
local TweenService = cloneref(game:GetService("TweenService"))
local RunService = cloneref(game:GetService("RunService"))
local HttpService = cloneref(game:GetService("HttpService"))
local TextService = cloneref(game:GetService("TextService"))
local diagnostics = _basaltEditorDiagnostics == true
-- Different game VM globals may schedule an editor on the same DataModel.
-- Keep the existing visible interface instead of creating another overlay.
local coreGui = game:GetService("CoreGui")
local existingHost, existingInterface
for _, candidate in coreGui:GetChildren() do
    if candidate:IsA("Folder") and candidate:GetAttribute("_b_ui") == true then
        for _, screen in candidate:GetChildren() do
            if screen:IsA("ScreenGui") and screen:GetAttribute("_b_main") == true then
                existingHost, existingInterface = candidate, screen
                break
            end
        end
        if existingInterface then break end
    end
end
if existingInterface and existingInterface:IsA("ScreenGui") and existingInterface.Parent == existingHost then
    existingInterface.Enabled = true
    return
end

local player = Players.LocalPlayer
while not player do
    task.wait(0.1)
    player = Players.LocalPlayer
end
local shared = getgenv()
local previous = diagnostics and shared.basaltEditor
local uiHost = existingHost
if not uiHost then
    uiHost = Instance.new("Folder")
    uiHost.Name = diagnostics and "Basalt_Internal" or HttpService:GenerateGUID(false)
    uiHost:SetAttribute("_b_ui",true)
    uiHost.Parent = coreGui
end
local openDiscordInvite = _openDiscordInvite or previous and previous.openDiscord
local editorInputBridge = _editorInput or previous and previous.editorInput
local serviceProtectionBridge = _serviceProtection or previous and previous.protectionControl
if previous and previous.gui and previous.gui.Parent then
    if previous.version == VERSION then
        previous.gui.Enabled = true
        if previous.dimGui then previous.dimGui.Enabled = true end
        return
    end
    if previous.destroy then pcall(previous.destroy) end
    if previous.gui.Parent then previous.gui:Destroy() end
end

local colors = {
    panel = Color3.fromRGB(38, 38, 38),
    code = Color3.fromRGB(29, 29, 29),
    selected = Color3.fromRGB(54, 54, 54),
    accent = Color3.fromRGB(245, 245, 245),
    ink = Color3.fromRGB(20, 20, 20),
    text = Color3.fromRGB(245, 245, 245),
    muted = Color3.fromRGB(153, 153, 153),
    edge = Color3.fromRGB(125, 125, 125),
}

local unitRoot, unitScale = nil, 1
local unitProperties = {}
local function pixelValue(property, value)
    if property == "Position" or property == "Size" or property == "CanvasSize" then
        return UDim2.new(value.X.Scale, math.round(value.X.Offset * unitScale), value.Y.Scale, math.round(value.Y.Offset * unitScale))
    elseif property == "CornerRadius" then
        return UDim.new(value.Scale, math.round(value.Offset * unitScale))
    elseif property == "TextSize" then
        return math.max(1, math.round(value * unitScale))
    elseif property == "Thickness" then
        return value * unitScale
    end
    return value
end
local function setUnit(item, property, value, animation)
    local properties = unitProperties[item]
    if not properties then
        properties = {}; unitProperties[item] = properties
        item.Destroying:Connect(function() unitProperties[item] = nil end)
    end
    properties[property] = value
    if animation then
        local tween = TweenService:Create(item, animation, {[property] = pixelValue(property, value)})
        tween:Play()
        return tween
    end
    item[property] = pixelValue(property, value)
end
local function design(item, property)
    return unitProperties[item] and unitProperties[item][property] or item[property]
end
local function rescale(factor)
    unitScale = factor
    for item, properties in unitProperties do
        if item.Parent then
            for property, value in properties do item[property] = pixelValue(property, value) end
        end
    end
end
local function new(class, name, parent, properties)
    local item = Instance.new(class)
    item.Name = name
    if item:IsA("GuiObject") then item.BorderSizePixel = 0 end
    local ancestor, scaled = parent, false
    while ancestor do
        if ancestor == unitRoot then scaled = true; break end
        ancestor = ancestor.Parent
    end
    for key, value in pairs(properties) do
        if scaled and (key == "Position" or key == "Size" or key == "CanvasSize" or key == "CornerRadius" or key == "TextSize" or key == "Thickness") then
            setUnit(item, key, value)
        else item[key] = value end
    end
    item.Parent = parent
    return item
end

local function rounded(item, radius)
    local corner = item:FindFirstChild("Corner")
    if corner then setUnit(corner, "CornerRadius", UDim.new(0, radius))
    else new("UICorner", "Corner", item, {CornerRadius = UDim.new(0, radius)}) end
    return item
end

local function surface(item, radius)
    rounded(item, radius)
    item.BackgroundTransparency = 0
    return item
end

local atlas = "rbxasset://textures/basalt/glass-icons.png"
local brandAsset = "rbxasset://textures/basalt/basalt-logo.png"
local iconIds = {home=0, terminal=1, gamepad=2, cloud=3, settings=4, folder=5, document=6,
    close=7, play=8, eraser=9, paste=10, copy=11, bolt=13, clock=14,
    hourglass=15, chevron=16, add=17, check=18, trash=19, discord=20,
    search=21, compass=22, flame=23, refresh=24, right=25, left=26,
    verified=27, key=28, keyless=29, user=30, alert=31, loader=32}
local function icon(parent, kind, size, position, color)
    if kind == "sparkles" or kind == "chevronDown" then
        local item=new("ImageLabel","Icon",parent,{BackgroundTransparency=1,Image="",ImageColor3=color or colors.text,
            Size=UDim2.fromOffset(size,size),Position=position,ZIndex=parent.ZIndex+1})
        local strokes={}
        local function line(x1,y1,x2,y2)
            local dx,dy=x2-x1,y2-y1
            local stroke=rounded(new("Frame","Stroke",item,{BackgroundColor3=item.ImageColor3,
                Size=UDim2.fromScale(math.sqrt(dx*dx+dy*dy),0.065),Position=UDim2.fromScale((x1+x2)/2,(y1+y2)/2),
                AnchorPoint=Vector2.new(0.5,0.5),Rotation=math.deg(math.atan2(dy,dx)),ZIndex=item.ZIndex+1}),1)
            strokes[#strokes+1]=stroke
        end
        if kind == "chevronDown" then line(.22,.38,.5,.66); line(.5,.66,.78,.38)
        else
            local function sparkle(cx,cy,r)
                local points={{0,-1},{.28,-.28},{1,0},{.28,.28},{0,1},{-.28,.28},{-1,0},{-.28,-.28}}
                for index,p in points do local q=points[index%#points+1]; line(cx+p[1]*r,cy+p[2]*r,cx+q[1]*r,cy+q[2]*r) end
            end
            sparkle(.4,.6,.35); sparkle(.77,.23,.18)
        end
        item:GetPropertyChangedSignal("ImageColor3"):Connect(function()
            for _,stroke in strokes do stroke.BackgroundColor3=item.ImageColor3 end
        end)
        return item
    end
    local index = assert(iconIds[kind], kind)
    return new("ImageLabel", "Icon", parent, {
        BackgroundTransparency = 1, Image = atlas,
        ImageRectOffset = Vector2.new(index % 5 * 64, math.floor(index / 5) * 64),
        ImageRectSize = Vector2.new(64, 64), ImageColor3 = color or colors.text,
        Size = UDim2.fromOffset(size, size), Position = position,
        ZIndex = parent.ZIndex + 1,
    })
end

local function label(name, parent, caption, size, position, fontSize, color)
    return new("TextLabel", name, parent, {
        BackgroundTransparency = 1, Text = caption, TextColor3 = color or colors.text,
        TextSize = fontSize or 10, Font = Enum.Font.BuilderSansMedium,
        TextXAlignment = Enum.TextXAlignment.Left, Size = size, Position = position,
        ZIndex = parent.ZIndex + 1,
    })
end

local function button(name, parent, caption, size, position, background, foreground)
    return rounded(new("TextButton", name, parent, {
        AutoButtonColor = false,
        Text = caption,
        TextColor3 = foreground or colors.text,
        TextSize = 10,
        Font = Enum.Font.BuilderSansMedium,
        BackgroundColor3 = background,
        BackgroundTransparency = 0,
        Size = size,
        Position = position,
        ZIndex = parent.ZIndex + 1,
    }), 12)
end

local gui = new("ScreenGui", diagnostics and "Interface" or HttpService:GenerateGUID(false), uiHost, {
    ResetOnSpawn = false,
    IgnoreGuiInset = true,
    ScreenInsets = Enum.ScreenInsets.DeviceSafeInsets,
    DisplayOrder = 71,
    ZIndexBehavior = Enum.ZIndexBehavior.Sibling,
})
gui:SetAttribute("_b_main",true)
local dimGui = new("ScreenGui", diagnostics and "Backdrop" or HttpService:GenerateGUID(false), uiHost, {
    ResetOnSpawn = false,
    IgnoreGuiInset = true,
    ScreenInsets = Enum.ScreenInsets.None,
    ClipToDeviceSafeArea = false,
    DisplayOrder = 70,
    ZIndexBehavior = Enum.ZIndexBehavior.Sibling,
})
local root = new("Frame", "Root", gui, {
    BackgroundTransparency = 1,
    Active = false,
    Size = UDim2.fromScale(1, 1),
})
local backdrop = new("TextButton", "Backdrop", dimGui, {
    BackgroundColor3 = Color3.new(0, 0, 0),
    BackgroundTransparency = 1,
    Size = UDim2.fromScale(1, 1),
    AutoButtonColor = false,
    Active = true,
    Text = "",
    Visible = false,
    ZIndex = 1,
})
local floating = button("Toggle", root, "", UDim2.fromOffset(48, 48),
    UDim2.new(0.5, -24, 0.17, 0), colors.panel, colors.text)
floating.Corner.CornerRadius = UDim.new(0.5, 0)
floating.BackgroundTransparency = 0.08
floating.Visible = false
floating.ZIndex = 10
local function brand(parent, size)
    return new("ImageLabel", "Logo", parent, {
        BackgroundTransparency=1, Image=brandAsset, ImageColor3=colors.text,
        Size=UDim2.fromOffset(size, size), Position=UDim2.fromScale(0.5, 0.5),
        AnchorPoint=Vector2.new(0.5, 0.5), ZIndex=parent.ZIndex + 1,
    })
end
brand(floating, 30)
floating.Logo.Size = UDim2.fromScale(29 / 45, 29 / 45)
local chrome = {home=UDim2.new(0.5, -24, 0.17, 0), moving=false, dragged=false, tweens={}}
chrome.close = icon(floating, "close", 18, UDim2.fromScale(0.5, 0.5))
chrome.close.AnchorPoint = Vector2.new(0.5, 0.5)
chrome.close.Size = UDim2.fromScale(16 / 40, 16 / 40)
chrome.close.ImageTransparency = 1

local panel = rounded(new("Frame", "Panel", root, {
    BackgroundTransparency = 1,
    Size = UDim2.fromOffset(536, 310),
    Position = UDim2.fromOffset(16, 16),
    Active = true,
    ClipsDescendants = false,
    ZIndex = 2,
}), 0)
local initialArea = root.AbsoluteSize
local initialScale = initialArea.X > 240 and math.min(initialArea.X / 704, math.max(150, initialArea.Y - 14) / 310, 2.6) or 1
if initialArea.X < 560 and initialArea.Y > 280 then initialScale = math.clamp(initialArea.X / 390, 0.82, 1.2) end
local scale = {Scale = initialScale}
unitRoot, unitScale = panel, initialScale
local closeButton = surface(button("Close", panel, "", UDim2.fromOffset(40, 40),
    UDim2.fromOffset(139, 0), colors.panel), 24)
closeButton.AnchorPoint = Vector2.new(0.5, 0)
closeButton.BackgroundTransparency = 0.08
local closeLogo = brand(closeButton, 29)
local closeSymbol = icon(closeButton, "close", 16, UDim2.fromScale(0.5, 0.5))
closeSymbol.AnchorPoint = Vector2.new(0.5, 0.5)
closeSymbol.ImageTransparency = 1
closeLogo.Visible = false
local dock = surface(new("Frame", "Dock", panel, {
    BackgroundColor3 = colors.panel, Size = UDim2.fromOffset(154, 40),
    Position = UDim2.fromOffset(248, 0), ZIndex = 3,
}), 24)
dock.BackgroundTransparency = 0.08
local navSelection = rounded(new("Frame", "Selection", dock, {
    BackgroundColor3 = colors.accent, Size = UDim2.fromOffset(34, 34),
    Position = UDim2.new(0, 20, 0.5, 0), AnchorPoint = Vector2.new(0.5, 0.5), ZIndex = 4,
}), 19)
local dockButtons = {}
for index, item in {{"CodeTab", "home"}, {"ConsoleTab", "terminal"}, {"SavedTab", "folder"}, {"SettingsTab", "settings"}} do
    local control = button(item[1], dock, "", UDim2.fromOffset(34, 34),
        UDim2.fromOffset(3 + (index - 1) * 38, 3), colors.accent)
    setUnit(control.Corner, "CornerRadius", UDim.new(1, 0))
    control.BackgroundTransparency = 1
    local symbol = icon(control, item[2], 23,
        UDim2.fromScale(0.5, 0.5), index == 1 and colors.ink or colors.text)
    symbol.AnchorPoint = Vector2.new(0.5, 0.5)
    dockButtons[#dockButtons + 1] = control
end
local codeTab, consoleTab = dockButtons[1], dockButtons[2]
local mainGlass = surface(new("Frame", "Glass", panel, {
    BackgroundColor3 = colors.panel, Position = UDim2.fromOffset(0, 77),
    Size = UDim2.fromOffset(536, 233), ZIndex = 3,
}), 17)
mainGlass.BackgroundTransparency = 0.08
local pillEntries = {}
local function statusPill(name, kind, width, position, text)
    local pill = rounded(new("Frame", name, panel, {
        BackgroundColor3 = colors.panel, BackgroundTransparency = 0.08,
        Size = UDim2.fromOffset(width, 22), Position = position, ZIndex = 3,
    }), 10)
    local symbol = icon(pill, kind, 13, UDim2.fromOffset(6, 3.5))
    local caption = label("Value", pill, text, UDim2.fromOffset(24, 22), UDim2.fromOffset(21, 0), 10)
    symbol.AnchorPoint, caption.AnchorPoint = Vector2.new(0, 0.5), Vector2.new(0, 0.5)
    caption.TextYAlignment = Enum.TextYAlignment.Center
    pillEntries[#pillEntries + 1] = {pill=pill, icon=symbol, text=caption}
    return pill, caption
end
local fpsPill, fpsText = statusPill("FrameRate", "bolt", 56, UDim2.fromOffset(0, 54), "- FPS")
local clockPill, clockText = statusPill("Clock", "clock", 62, UDim2.fromOffset(474, 54), "")
chrome.discord = surface(button("Discord", panel, "", UDim2.fromOffset(26, 18), UDim2.fromOffset(103, 54), colors.panel), 10)
chrome.discord.BackgroundTransparency = 0.08
icon(chrome.discord, "discord", 12, UDim2.fromOffset(7, 3))

local tabs = {}
local activeScript = 1
do
    local ok, contents = pcall(readfile, "draft.luau")
    tabs[1] = {name = "New Script",
        text = ok and type(contents) == "string" and #contents <= MAX_SCRIPT_BYTES and contents or DEFAULT_SCRIPT,
        path = "draft.luau"}
end
local createdTabs = 1
pcall(function()
    local encoded = readfile(".basalt/tabs.json")
    if #encoded > 65536 then return end -- room for 24 tab metadata entries
    local session = HttpService:JSONDecode(encoded)
    if type(session.tabs) ~= "table" or #session.tabs < 1 or #session.tabs > 24 then return end
    local restored = {}
    for _, tab in session.tabs do
        if type(tab.name) ~= "string" or #tab.name > 160 or type(tab.path) ~= "string" or
            not (tab.path == "draft.luau" or tab.path:match("^%.basalt/tab%-%d+%.luau$")) then return end
        local ok, contents = pcall(readfile, tab.path)
        restored[#restored + 1] = {name=tab.name, path=tab.path,
            savedId=type(tab.savedId)=="string" and #tab.savedId<=48 and tab.savedId:match("^[%w%-]+$") and tab.savedId or nil,
            text=ok and type(contents) == "string" and #contents <= MAX_SCRIPT_BYTES and contents or ""}
        createdTabs = math.max(createdTabs, tonumber(tab.path:match("tab%-(%d+)")) or 0)
    end
    tabs = restored
    if type(session.created) == "number" and session.created >= 0 and session.created < 1000000000 then
        createdTabs = math.max(createdTabs, math.floor(session.created))
    end
    activeScript = math.clamp(tonumber(session.active) or 1, 1, #tabs)
    activeScript = math.floor(activeScript)
end)
local tabStrip = new("ScrollingFrame", "ScriptTabs", panel, {
    BackgroundTransparency = 1, Position = UDim2.fromOffset(12, 89),
    Size = UDim2.fromOffset(512, 31), CanvasSize = UDim2.fromOffset(0, 0),
    CanvasPosition = Vector2.new(0, 0),
    ScrollBarThickness = 0, ScrollingDirection = Enum.ScrollingDirection.X, ZIndex = 5,
})
local addButton = surface(button("AddTab", tabStrip, "", UDim2.fromOffset(30, 30),
    UDim2.fromOffset(348, 1), colors.selected, colors.text), 15)
setUnit(addButton.Corner, "CornerRadius", UDim.new(0.5, 0))
icon(addButton, "add", 16, UDim2.fromOffset(7, 7))
local consoleCopy = surface(button("ConsoleCopy", panel, "", UDim2.fromOffset(30, 30),
    UDim2.fromOffset(444, 86), colors.selected), 15)
icon(consoleCopy, "copy", 14, UDim2.fromScale(0.5, 0.5)).AnchorPoint = Vector2.new(0.5, 0.5)
consoleCopy.Visible = false
local consoleClear = surface(button("ConsoleClear", panel, "", UDim2.fromOffset(30, 30),
    UDim2.fromOffset(479, 86), colors.selected), 15)
setUnit(consoleClear.Corner, "CornerRadius", UDim.new(0.5, 0))
icon(consoleClear, "trash", 14, UDim2.fromScale(0.5, 0.5)).AnchorPoint = Vector2.new(0.5, 0.5)
consoleClear.Visible = false

local editorCard = rounded(new("Frame", "EditorCard", panel, {
    BackgroundTransparency = 1,
    Position = UDim2.fromOffset(12, 128),
    Size = UDim2.fromOffset(512, 144),
    ClipsDescendants = true,
    ZIndex = 4,
}), 0)
local scroll = new("ScrollingFrame", "EditorScroll", editorCard, {
    BackgroundTransparency = 1,
    Position = UDim2.fromOffset(0, 0),
    Size = UDim2.fromScale(1, 1),
    CanvasSize = UDim2.fromOffset(0, 0),
    ScrollingDirection = Enum.ScrollingDirection.XY,
    ScrollBarThickness = 2,
    ScrollBarImageColor3 = colors.muted,
    ScrollBarImageTransparency = 0.55, ZIndex = 4,
})
local editor = new("TextBox", "Source", scroll, {
    BackgroundTransparency = 1,
    Position = UDim2.fromOffset(37, 3),
    Size = UDim2.new(1, -43, 0, 160),
    ClearTextOnFocus = false,
    ShowNativeInput = true,
    MultiLine = true,
    TextWrapped = false,
    Text = tabs[activeScript].text,
    PlaceholderText = "",
    PlaceholderColor3 = colors.muted,
    TextColor3 = colors.text,
    TextTransparency = 1,
    Font = Enum.Font.Code,
    TextSize = 13,
    LineHeight = 1.13, ZIndex = 6,
    TextXAlignment = Enum.TextXAlignment.Left,
    TextYAlignment = Enum.TextYAlignment.Top,
})
-- Native input, highlighting, and scrolling share the same pixel coordinates.
local codeFontSize, codeFontScale = 13, 1
local highlighting = label("Highlight", scroll, "", design(editor, "Size"), design(editor, "Position"), 13)
highlighting.Font = Enum.Font.Code
highlighting.TextYAlignment = Enum.TextYAlignment.Top
highlighting.RichText = true
highlighting.LineHeight = 1.13
highlighting.ZIndex = 7
highlighting.Active = false
local caret = new("Frame", "Caret", scroll, {
    BackgroundColor3=colors.text, BackgroundTransparency=0, Size=UDim2.fromOffset(1, 13),
    Position=UDim2.fromOffset(37, 3), Visible=false, ZIndex=8,
})
local lineNumbers = label("LineNumbers", scroll, "1", UDim2.fromOffset(20, 160), UDim2.fromOffset(0, 3), 13, colors.muted)
lineNumbers.Font = Enum.Font.Code
lineNumbers.TextXAlignment = Enum.TextXAlignment.Right
lineNumbers.TextYAlignment = Enum.TextYAlignment.Top
lineNumbers.LineHeight = 1.13
local consoleCard = rounded(new("Frame", "ConsoleCard", panel, {
    BackgroundColor3 = colors.panel, BackgroundTransparency = 0.08,
    Position = UDim2.fromOffset(0, 77),
    Size = UDim2.fromOffset(536, 233),
    ClipsDescendants = true,
    Visible = false,
    ZIndex = 5,
}), 17)
consoleCopy.Parent, consoleClear.Parent = consoleCard, consoleCard
consoleCopy.ZIndex, consoleClear.ZIndex = 9, 9
consoleCopy.Icon.ZIndex, consoleClear.Icon.ZIndex = 10, 10
setUnit(consoleCopy, "Position", UDim2.new(1, -76, 0, 12))
setUnit(consoleClear, "Position", UDim2.new(1, -40, 0, 12))
local consoleScroll = new("ScrollingFrame", "ConsoleScroll", consoleCard, {
    BackgroundTransparency = 1,
    Position = UDim2.fromOffset(16, 14),
    Size = UDim2.new(1, -104, 1, -28),
    CanvasSize = UDim2.fromOffset(0, 0),
    AutomaticCanvasSize = Enum.AutomaticSize.None,
    ScrollingDirection = Enum.ScrollingDirection.Y,
    CanvasPosition = Vector2.new(0, 0),
    ScrollBarThickness = 3,
    ScrollBarImageColor3 = colors.muted,
    ZIndex = 5,
})
local output = new("TextLabel", "ConsoleText", consoleScroll, {
    BackgroundTransparency = 1,
    Text = "",
    TextColor3 = colors.text,
    Font = Enum.Font.Code,
    TextSize = 12,
    LineHeight = 1.2,
    TextWrapped = true,
    TextXAlignment = Enum.TextXAlignment.Left,
    TextYAlignment = Enum.TextYAlignment.Top,
    Position = UDim2.fromOffset(0, 0),
    Size = UDim2.new(1, -12, 0, 0),
    AutomaticSize = Enum.AutomaticSize.None,
    ZIndex = 6,
    Visible = false,
})
local consoleDisplay = label("FormattedOutput", consoleScroll, "", UDim2.new(1, -12, 0, 0), UDim2.fromOffset(0, 0), 12)
consoleDisplay.Font = Enum.Font.Code
consoleDisplay.TextYAlignment = Enum.TextYAlignment.Top
consoleDisplay.TextWrapped, consoleDisplay.RichText, consoleDisplay.LineHeight = true, true, 1.2
local latestButton = surface(button("Latest", consoleCard, "", UDim2.fromOffset(28, 28), UDim2.new(1, -40, 1, -40), colors.selected), 14)
latestButton.ZIndex, latestButton.Visible = 9, false
icon(latestButton, "chevron", 14, UDim2.fromScale(0.5, 0.5)).AnchorPoint = Vector2.new(0.5, 0.5)
local actions = new("Frame", "Actions", panel, {
    BackgroundTransparency = 1,
    Position = UDim2.fromOffset(12, 271),
    Size = UDim2.fromOffset(512, 31),
    ZIndex = 5,
})
local runButton = surface(button("Run", actions, "", UDim2.fromOffset(100, 31), UDim2.fromOffset(0, 0), colors.accent), 16)
runButton.AnchorPoint = Vector2.new(1, 0)
setUnit(runButton, "Position", UDim2.fromScale(1, 0))
setUnit(runButton.Corner, "CornerRadius", UDim.new(0.5, 0))
local runCaption = label("Caption", runButton, "Execute", UDim2.fromScale(1, 1), UDim2.new(0, 0, 0.5, 0), 10, colors.ink)
runCaption.TextXAlignment = Enum.TextXAlignment.Center
runCaption.AnchorPoint = Vector2.new(0, 0.5 + 0.65 / 31)
runCaption.Font = Enum.Font.BuilderSansBold
local runSymbol = icon(runButton, "play", 12, UDim2.new(0, 16, 0.5, 0), colors.ink)
runSymbol.AnchorPoint = Vector2.new(0, 0.5)
setUnit(runCaption, "Position", UDim2.new(0, 20, 0.5, 0))
setUnit(runCaption, "Size", UDim2.new(1, -20, 1, 0))
local editActions = surface(new("Frame", "EditActions", actions, {
    BackgroundColor3=colors.selected, Size=UDim2.fromOffset(166, 31),
    Position=UDim2.fromOffset(0, 0), ZIndex=5,
}), 16)
editActions.AnchorPoint = Vector2.new(1, 0)
setUnit(editActions.Corner, "CornerRadius", UDim.new(0.5, 0))
local clipboardButton = surface(button("Paste", editActions, "", UDim2.fromOffset(83, 31), UDim2.fromOffset(166, 0), colors.selected), 16)
clipboardButton.AnchorPoint = Vector2.new(1, 0)
setUnit(clipboardButton.Corner, "CornerRadius", UDim.new(0.5, 0))
clipboardButton.BackgroundTransparency = 1
local clipboardCaption = label("Caption", clipboardButton, "Paste", UDim2.fromOffset(60, 31), UDim2.new(0, 24, 0.5, 0), 10)
clipboardCaption.TextXAlignment = Enum.TextXAlignment.Center
clipboardCaption.AnchorPoint = Vector2.new(0, 0.5 + 0.65 / 31)
clipboardCaption.Font = Enum.Font.BuilderSansBold
local clipboardSymbol = icon(clipboardButton, "paste", 12, UDim2.new(0, 12, 0.5, 0))
clipboardSymbol.AnchorPoint = Vector2.new(0, 0.5)
local executeClipboardButton = surface(button("ExecuteClipboard", actions, "", UDim2.fromOffset(144, 31), UDim2.new(1, -106, 0, 0), colors.selected), 16)
executeClipboardButton.AnchorPoint = Vector2.new(1, 0)
setUnit(executeClipboardButton.Corner, "CornerRadius", UDim.new(0.5, 0))
local executeClipboardCaption = label("Caption", executeClipboardButton, "Execute Clipboard", UDim2.fromOffset(110, 31), UDim2.new(0, 24, 0.5, 0), 10)
executeClipboardCaption.TextXAlignment = Enum.TextXAlignment.Center
executeClipboardCaption.AnchorPoint = Vector2.new(0, 0.5 + 0.65 / 31)
executeClipboardCaption.Font = Enum.Font.BuilderSansBold
local executeClipboardSymbol = icon(executeClipboardButton, "paste", 12, UDim2.new(0, 12, 0.5, 0))
executeClipboardSymbol.AnchorPoint = Vector2.new(0, 0.5)
local clearButton = surface(button("ClearEditor", editActions, "Clear", UDim2.fromOffset(83, 31),
    UDim2.fromOffset(0, 0), colors.selected), 16)
clearButton.AnchorPoint = Vector2.new(0, 0)
setUnit(clearButton.Corner, "CornerRadius", UDim.new(0.5, 0))
clearButton.BackgroundTransparency = 1
clearButton.Font = Enum.Font.BuilderSansBold
clearButton.TextSize = 10
local editDivider = new("Frame", "Divider", editActions, {
    BackgroundColor3=colors.edge, BackgroundTransparency=0.68,
    Size=UDim2.fromOffset(1, 17), Position=UDim2.fromOffset(83, 7),
    Active=false, ZIndex=8,
})
function chrome.alignExecute()
    for _, entry in {{runCaption, runSymbol}, {clipboardCaption, clipboardSymbol}, {executeClipboardCaption, executeClipboardSymbol}} do
        local caption, symbol = entry[1], entry[2]
        local buttonWidth = design(caption.Parent, "Size").X.Offset
        local width = TextService:GetTextSize(caption.Text, caption.TextSize, caption.Font, Vector2.new(1000, 100)).X / unitScale
        local left = (buttonWidth - width - 19) / 2
        setUnit(symbol, "Position", UDim2.new(0, left, 0.5, 0))
        setUnit(caption, "Position", UDim2.new(0, left + 19, 0.5, 0))
        setUnit(caption, "Size", UDim2.fromOffset(width, 31))
    end
end
local blur = new("BlurEffect", "BlurEffect", workspace.CurrentCamera, {Size = 0, Enabled = true})

local subscriptions = {}
local function observe(signal, callback)
    local connection = signal:Connect(callback)
    subscriptions[#subscriptions + 1] = connection
    return connection
end
local function buttonMotion(control, connections)
    local feedback = new("Frame", "Press", control, {
        BackgroundColor3=colors.text, BackgroundTransparency=1, Size=UDim2.fromScale(1, 1),
        Active=false, ZIndex=control.ZIndex + 3,
    })
    new("UICorner", "Corner", feedback, {CornerRadius=UDim.new(0.5, 0)})
    local hovering, down, revision, tween = false, false, 0, nil
    local function animatePress()
        if tween then tween:Cancel() end
        feedback.BackgroundColor3 = control.BackgroundColor3 == colors.accent and colors.ink or colors.text
        tween = TweenService:Create(feedback, TweenInfo.new(down and 0.07 or 0.18, Enum.EasingStyle.Quad, Enum.EasingDirection.Out),
            {BackgroundTransparency=down and 0.87 or hovering and 0.96 or 1})
        tween:Play()
    end
    local function bind(signal, callback)
        local connection = observe(signal, callback)
        if connections then connections[#connections + 1] = connection end
    end
    bind(control.MouseEnter, function() hovering = true; animatePress() end)
    bind(control.MouseLeave, function() hovering, down = false, false; animatePress() end)
    bind(control.InputBegan, function(input)
        if input.UserInputType == Enum.UserInputType.Touch or input.UserInputType == Enum.UserInputType.MouseButton1 then
            down = true; animatePress()
        end
    end)
    bind(control.InputEnded, function(input)
        if input.UserInputType == Enum.UserInputType.Touch or input.UserInputType == Enum.UserInputType.MouseButton1 then
            down = false; animatePress()
        end
    end)
    bind(control.Activated, function()
        revision += 1
        local current = revision
        down = true; animatePress()
        task.delay(0.08, function()
            if current == revision and control.Parent then down = false; animatePress() end
        end)
    end)
end
for _, control in {runButton, clipboardButton, executeClipboardButton, clearButton, consoleClear, consoleCopy, latestButton, addButton, chrome.discord, codeTab, consoleTab, dockButtons[3], dockButtons[4]} do buttonMotion(control) end
local settings = createEditorSettings({new=new, rounded=rounded, button=button, label=label, icon=icon, setUnit=setUnit,
    colors=colors, observe=observe, panel=panel, buttonMotion=buttonMotion, http=HttpService,
    protection=serviceProtectionBridge, reportError=_reportScriptError, scale=function() return unitScale end,
    request=request, shared=shared})
local preferences = settings.get()
codeFontSize = preferences.textSize
local function resetBrand(closing)
    closeLogo.ImageTransparency, closeSymbol.ImageTransparency = 1, 0
end
observe(chrome.discord.Activated, function()
    local ok, problem = pcall(openDiscordInvite)
    if not ok then _reportScriptError("Discord invite: " .. tostring(problem)) end
end)
local function alignPill(entry)
    local width = TextService:GetTextSize(entry.text.Text, entry.text.TextSize, entry.text.Font, Vector2.new(1000, 100)).X / unitScale
    local gap = math.max(4, 4 / unitScale)
    local symbol = design(entry.icon, "Size").X.Offset
    local start = (design(entry.pill, "Size").X.Offset - width - symbol - gap) / 2
    setUnit(entry.icon, "Position", UDim2.new(0, start, 0.5, 0))
    setUnit(entry.text, "Size", UDim2.fromOffset(width, design(entry.pill, "Size").Y.Offset))
    setUnit(entry.text, "Position", UDim2.new(0, start + symbol + gap, 0.5, -0.65))
end
for _, entry in pillEntries do
    alignPill(entry)
    observe(entry.text:GetPropertyChangedSignal("Text"), function() alignPill(entry) end)
end
local function persist()
    if editor.Parent and #editor.Text <= MAX_SCRIPT_BYTES then
        tabs[activeScript].text = editor.Text
        pcall(writefile, tabs[activeScript].path, editor.Text)
        pcall(function()
            local metadata = {}
            for _, tab in tabs do metadata[#metadata + 1] = {name=tab.name, path=tab.path, savedId=tab.savedId} end
            writefile(".basalt/tabs.json", HttpService:JSONEncode({version=2, tabs=metadata, active=activeScript, created=createdTabs}))
        end)
    end
end
local focused, expanded = false, true
local keywords = {}
for word in ("and break continue do else elseif end export false for function if in local nil not or repeat return then true type typeof until while"):gmatch("%S+") do
    keywords[word] = true
end
local builtins = {getgenv=true, getfenv=true, setfenv=true, print=true, warn=true, pairs=true, ipairs=true, tostring=true, tonumber=true, loadstring=true}
local function escape(text)
    return text:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"):gsub('"', "&quot;")
end
local function highlight(text, firstByte, lastByte)
    firstByte, lastByte = firstByte or 1, lastByte or #text
    local pieces, cursor = {}, 1
    while cursor <= lastByte do
        local first = text:sub(cursor, cursor)
        local token, color
        if text:sub(cursor, cursor + 1) == "--" then
            local bracket = text:match("^%-%-%[(=*)%[", cursor)
            local ending = bracket and text:find("]" .. bracket .. "]", cursor + 4 + #bracket, true)
            local stop = bracket and (ending and ending + #bracket + 1 or #text) or ((text:find("\n", cursor, true) or (#text + 1)) - 1)
            token, color = text:sub(cursor, stop), "879a89"
        elseif text:match("^%[(=*)%[", cursor) then
            local bracket = text:match("^%[(=*)%[", cursor)
            local ending = text:find("]" .. bracket .. "]", cursor + 2 + #bracket, true)
            token, color = text:sub(cursor, ending and ending + #bracket + 1 or #text), "c1d9b1"
        elseif first == '"' or first == "'" or first == "`" then
            local quote, stop = first, cursor + 1
            while stop <= #text do
                local char = text:sub(stop, stop)
                if char == "\\" then stop += 2
                elseif char == quote then stop += 1; break
                else stop += 1 end
            end
            token, color = text:sub(cursor, stop - 1), "c1d9b1"
        else
            token = text:match("^[%a_][%w_]*", cursor)
            if token then
                color = keywords[token] and "b4d7f4" or
                    (token == "string" or token == "table" or token == "math" or text:sub(cursor - 1, cursor - 1) == ".") and "e6dcb2" or
                    (builtins[token] or text:sub(cursor + #token, cursor + #token) == "(") and "dfa5a4" or nil
            else
                token = text:match("^0[xX][%da-fA-F_]+", cursor) or text:match("^%d[%d_%.]*", cursor)
                if token then color = "dca9cb" else token = first end
            end
        end
        local first, last = math.max(1, firstByte - cursor + 1), math.min(#token, lastByte - cursor + 1)
        if first <= last then
            local visible = token:sub(first, last)
            pieces[#pieces + 1] = color and ('<font color="#' .. color .. '">' .. escape(visible) .. '</font>') or escape(visible)
        end
        cursor += #token
    end
    return table.concat(pieces)
end
local caretClock = 0
local nativeInput = {active=false, selecting=false, updating=false, elapsed=0}
local renderWindow = {starts={1}, text=nil, first=nil, last=nil}
local function renderVisibleCode()
    lineNumbers.Visible = preferences.lineNumbers
    if not editor.Parent or #renderWindow.starts == 0 then return end
    local pitch = editor.TextSize * editor.LineHeight
    local first = math.clamp(math.floor(scroll.CanvasPosition.Y / pitch) - 1, 1, #renderWindow.starts)
    local last = math.min(#renderWindow.starts, first + math.ceil(scroll.AbsoluteWindowSize.Y / pitch) + 3)
    if renderWindow.first == first and renderWindow.last == last and renderWindow.text == editor.Text then return end
    renderWindow.first, renderWindow.last, renderWindow.text = first, last, editor.Text
    local begin = renderWindow.starts[first]
    local finish = (renderWindow.starts[last + 1] or (#editor.Text + 2)) - 2
    local visible = editor.Text:sub(begin, finish)
    local colored = preferences.syntaxHighlighting and #editor.Text < 65536 and #visible < 16000
    highlighting.RichText = colored
    highlighting.Visible = true
    highlighting.TextColor3 = editor.Text == "" and colors.muted or colors.text
    highlighting.Text = editor.Text == "" and "Write or paste Luau here..." or (colored and highlight(editor.Text, begin, finish) or visible)
    local numbers = {}
    for index = first, last do numbers[#numbers + 1] = tostring(index) end
    lineNumbers.Text = table.concat(numbers, "\n")
    local y = (first - 1) * pitch / unitScale + 3
    setUnit(highlighting, "Position", UDim2.fromOffset(37, y))
    setUnit(lineNumbers, "Position", UDim2.fromOffset(0, y))
end
local function updateCaret(reveal)
    local cursor = editor.CursorPosition
    if nativeInput.active and nativeInput.state[21] and nativeInput.state[21] >= 0 then cursor = nativeInput.state[21] + 1 end
    caret.Visible = focused and cursor > 0 and not nativeInput.selecting and (editor.SelectionStart < 1 or editor.SelectionStart == cursor)
    if not caret.Visible then return end
    local prefix = editor.Text:sub(1, cursor - 1)
    local _, row = prefix:gsub("\n", "")
    local line = prefix:match("[^\n]*$") or ""
    local width = TextService:GetTextSize(line, editor.TextSize, editor.Font, Vector2.new(1000000, 1000)).X
    local x = editor.Position.X.Offset + width
    local y = editor.Position.Y.Offset + row * editor.TextSize * editor.LineHeight
    if nativeInput.active and nativeInput.state[20] and nativeInput.state[2] > 0 then
        x = editor.Position.X.Offset + nativeInput.state[20] / nativeInput.state[2]
        y = editor.Position.Y.Offset + nativeInput.state[16] / nativeInput.state[2]
    end
    -- Document offsets are already scaled; UDim's integer offsets would truncate
    -- them once before scaling and again afterward if routed through setUnit.
    if unitProperties[caret] then unitProperties[caret].Position = nil end
    caret.Position = UDim2.fromOffset(math.round(x), math.round(y))
    setUnit(caret, "Size", UDim2.fromOffset(math.max(1, 1.3 / unitScale), editor.TextSize / unitScale))
    caretClock = os.clock()
    if reveal then
        local at, window = scroll.CanvasPosition, scroll.AbsoluteWindowSize
        local targetX, targetY = at.X, at.Y
        if x < at.X + 37 * unitScale then targetX = math.max(0, x - 37 * unitScale)
        elseif x > at.X + window.X - 8 then targetX = x - window.X + 8 end
        if y < at.Y then targetY = y
        elseif y + editor.TextSize > at.Y + window.Y - 4 then targetY = y + editor.TextSize - window.Y + 4 end
        scroll.CanvasPosition = Vector2.new(math.max(0, targetX), math.max(0, targetY))
    end
end
local function updateEditorHeight()
    if not editor.Parent then return end
    local lines, longest, offset = {}, 0, 1
    renderWindow.starts = {}
    for line in (editor.Text .. "\n"):gmatch("(.-)\n") do
        lines[#lines + 1] = tostring(#lines + 1)
        renderWindow.starts[#renderWindow.starts + 1] = offset
        offset += #line + 1
        longest = math.max(longest, #line)
    end
    local fontSize = codeFontSize * codeFontScale
    setUnit(editor, "TextSize", fontSize)
    local height = math.max(1, scroll.AbsoluteWindowSize.Y / unitScale - 3, #lines * editor.TextSize / unitScale * editor.LineHeight + 12)
    local width = math.max(1, scroll.AbsoluteWindowSize.X / unitScale - 43, longest * fontSize * 0.55 + 14)
    setUnit(editor, "TextSize", fontSize)
    setUnit(editor, "Size", UDim2.fromOffset(width, height))
    setUnit(highlighting, "Size", UDim2.fromOffset(width, scroll.AbsoluteWindowSize.Y / unitScale + 8 * fontSize * editor.LineHeight))
    setUnit(highlighting, "TextSize", fontSize)
    setUnit(lineNumbers, "Size", UDim2.fromOffset(20, scroll.AbsoluteWindowSize.Y / unitScale + 8 * fontSize * editor.LineHeight))
    setUnit(lineNumbers, "TextSize", fontSize)
    setUnit(scroll, "CanvasSize", UDim2.fromOffset(width + 43, height + 3))
    editor.TextTransparency = 1
    renderWindow.first = nil
    renderVisibleCode()
    updateCaret(false)
    if nativeInput.sync then nativeInput.sync() end
end
local function screenOffset()
    local ok, area = pcall(function() return GuiService:GetInsetArea(Enum.ScreenInsets.None) end)
    if ok then return Vector2.new(-area.Min.X, -area.Min.Y) end
    return GuiService:GetGuiInset()
end
function nativeInput.sync()
    if not editorInputBridge or nativeInput.updating or not editor.Parent then return end
    nativeInput.updating = true
    local origin, extent = editor.AbsolutePosition, editor.AbsoluteSize
    local viewport, window = scroll.AbsolutePosition, scroll.AbsoluteWindowSize
    local inset = screenOffset()
    origin, viewport = origin + inset, viewport + inset
    local x, y = math.max(origin.X, viewport.X), math.max(origin.Y, viewport.Y)
    local w = math.max(0, math.min(origin.X + extent.X, viewport.X + window.X) - x)
    local h = math.max(0, math.min(origin.Y + extent.Y, viewport.Y + window.Y) - y)
    local enabled = expanded and panel.Visible and editorCard.Visible and gui.Enabled
    local values = table.pack(pcall(editorInputBridge, x, y, w, h, origin.X, origin.Y, enabled))
    if values[1] then
        local state = {}
        for index = 2, values.n do state[index - 1] = values[index] end
        nativeInput.state = state
        nativeInput.active = focused and enabled and state[1] == 1
        nativeInput.selecting = nativeInput.active and state[6] >= 0 and state[7] >= 0 and state[6] ~= state[7]
        if nativeInput.active and state[2] > 0 and state[8] == state[9] then
            local dx, dy = state[3] / state[2] - (x - origin.X), state[4] / state[2] - (y - origin.Y)
            if math.abs(dx) > 1 / state[2] or math.abs(dy) > 1 / state[2] then
                local at = scroll.CanvasPosition
                scroll.CanvasPosition = Vector2.new(math.max(0, at.X + dx), math.max(0, at.Y + dy))
            end
        end
        if nativeInput.active and state[2] > 0 and state[5] > 0 then
            local pitch = state[5] / (editor.TextSize * state[2])
            if pitch >= 1 and pitch <= 1.4 and math.abs(editor.LineHeight - pitch) > 0.00001 then
                editor.LineHeight, highlighting.LineHeight, lineNumbers.LineHeight = pitch, pitch, pitch
                updateEditorHeight()
            end
        end
        updateCaret(false)
    end
    nativeInput.updating = false
end
observe(scroll:GetPropertyChangedSignal("CanvasPosition"), function() renderVisibleCode(); nativeInput.sync() end)
observe(editor:GetPropertyChangedSignal("AbsolutePosition"), nativeInput.sync)
observe(scroll:GetPropertyChangedSignal("AbsoluteWindowSize"), nativeInput.sync)
observe(panel:GetPropertyChangedSignal("Visible"), nativeInput.sync)
observe(editorCard:GetPropertyChangedSignal("Visible"), nativeInput.sync)
observe(RunService.RenderStepped, function(delta)
    nativeInput.elapsed += delta
    if focused and nativeInput.elapsed >= 1 / 30 then
        nativeInput.elapsed = 0
        nativeInput.sync()
    end
end)
local draftVersion = 0
local trackedText, trackedTab = editor.Text, tabs[activeScript]
local lastCursor, lastSelection = #editor.Text + 1, -1
local historyTime = -1
local function remember(stack, text, cursor)
    stack[#stack + 1] = {text=text, cursor=cursor}
    local bytes = 0
    for _, entry in stack do bytes += #entry.text end
    while #stack > 1 and (#stack > 32 or bytes > 512 * 1024) do
        bytes -= #table.remove(stack, 1).text
    end
end
local function recordEdit()
    local tab, text = tabs[activeScript], editor.Text
    if tab ~= trackedTab then
        trackedTab, trackedText, historyTime = tab, text, -1
        return
    end
    if text == trackedText then return end
    tab.undo, tab.redo = tab.undo or {}, {}
    if os.clock() - historyTime > 0.6 or math.abs(#text - #trackedText) > 1 then
        remember(tab.undo, trackedText, lastCursor)
    end
    trackedText, historyTime = text, os.clock()
end
local function setCode(text, cursor, record)
    local tab = tabs[activeScript]
    if record and text ~= editor.Text then
        tab.undo, tab.redo = tab.undo or {}, {}
        remember(tab.undo, editor.Text, lastCursor)
    end
    trackedTab, trackedText, historyTime = tab, text, os.clock()
    editor.Text = text
    lastCursor, lastSelection = math.clamp(cursor or (#text + 1), 1, #text + 1), -1
    editor.SelectionStart = -1
    if focused then editor.CursorPosition = lastCursor end
    updateEditorHeight()
    persist()
end
local function replaceSelection(value)
    local cursor = editor.CursorPosition > 0 and editor.CursorPosition or lastCursor
    local selection = editor.SelectionStart > 0 and editor.SelectionStart or lastSelection
    if nativeInput.active and nativeInput.state[21] and nativeInput.state[21] >= 0 then
        cursor, selection = nativeInput.state[21] + 1, nativeInput.state[22] + 1
    end
    local first, last = cursor, cursor
    if selection > 0 then first, last = math.min(cursor, selection), math.max(cursor, selection) end
    first, last = math.clamp(first, 1, #editor.Text + 1), math.clamp(last, 1, #editor.Text + 1)
    if #editor.Text - (last - first) + #value > MAX_SCRIPT_BYTES then return false, "Paste would exceed the editor's 8 MiB limit" end
    setCode(editor.Text:sub(1, first - 1) .. value .. editor.Text:sub(last), first + #value, true)
    editor:CaptureFocus()
    task.defer(function() if editor.Parent then editor.CursorPosition = lastCursor end end)
    return true
end
local function undoEdit(redo)
    recordEdit()
    local tab = tabs[activeScript]
    tab.undo, tab.redo = tab.undo or {}, tab.redo or {}
    local from, to = redo and tab.redo or tab.undo, redo and tab.undo or tab.redo
    local state = table.remove(from)
    if not state then return end
    remember(to, editor.Text, lastCursor)
    setCode(state.text, state.cursor, false)
end
local function indentation(line)
    if not preferences.autoIndent then return "" end
    local whitespace = line:match("^%s*") or ""
    if line:match("then%s*$") or line:match("do%s*$") or line:match("else%s*$") or line:match("repeat%s*$") or
        line:match("function.*%)%s*$") or line:match("[%{%(%[]%s*$") then
        whitespace ..= "    "
    end
    return whitespace
end
observe(editor:GetPropertyChangedSignal("Text"), function()
    focused = editor:IsFocused()
    local before, after = trackedText, editor.Text
    recordEdit()
    updateEditorHeight()
    draftVersion += 1
    local version = draftVersion
    task.delay(0.25, function()
        if version == draftVersion then persist() end
    end)
    if focused and editorInputBridge then
        task.defer(function()
            if editor.Parent and focused and draftVersion == version then nativeInput.sync(); updateCaret(not nativeInput.selecting) end
        end)
    end
    if focused and #after == #before + 1 then
        local at = 1
        while at <= #before and before:byte(at) == after:byte(at) do at += 1 end
        if after:sub(at, at) == "\n" and after:sub(at + 1) == before:sub(at) then
            local whitespace = indentation(after:sub(1, at - 1):match("[^\n]*$") or "")
            if #whitespace > 0 then
                task.defer(function()
                    if editor.Parent and editor.Text == after and draftVersion == version then
                        setCode(after:sub(1, at) .. whitespace .. after:sub(at + 1), at + 1 + #whitespace, false)
                    end
                end)
            end
        end
    end
end)
observe(editor.Focused, function()
    if not editor:IsFocused() then return end
    focused = true
    updateEditorHeight()
    task.defer(function() if editor.Parent then nativeInput.sync(); updateCaret(not editorInputBridge) end end)
end)
observe(editor.FocusLost, function(enterPressed)
    -- Deferred focus events can arrive after a release/capture in the same frame.
    if editor:IsFocused() then return end
    if nativeInput.active and nativeInput.state[21] and nativeInput.state[21] >= 0 then
        lastCursor, lastSelection = nativeInput.state[21] + 1, nativeInput.state[22] + 1
    end
    focused = false
    nativeInput.active, nativeInput.selecting = false, false
    updateEditorHeight()
    persist()
    if not enterPressed or not expanded or not editorCard.Visible then return end
    local before, cursor, selection = editor.Text, lastCursor, lastSelection
    task.defer(function()
        if not editor.Parent or not expanded or not editorCard.Visible or editor.Text ~= before then return end
        local first, last = cursor, cursor
        if selection > 0 then first, last = math.min(cursor, selection), math.max(cursor, selection) end
        first, last = math.clamp(first, 1, #before + 1), math.clamp(last, 1, #before + 1)
        local prefix = before:sub(1, first - 1)
        local newline = "\n" .. indentation(prefix:match("[^\n]*$") or "")
        setCode(prefix .. newline .. before:sub(last), first + #newline, true)
        editor:CaptureFocus()
        editor.CursorPosition = first + #newline
    end)
end)
observe(editor:GetPropertyChangedSignal("CursorPosition"), function()
    local cursor = editor.CursorPosition
    if cursor > 0 then lastCursor = cursor end
    updateCaret(not editorInputBridge)
end)
observe(editor:GetPropertyChangedSignal("SelectionStart"), function()
    if focused and editor.CursorPosition > 0 then lastSelection = editor.SelectionStart end
    updateCaret(false)
end)
observe(UserInputService.InputBegan, function(input)
    if focused and input.KeyCode == Enum.KeyCode.Tab then replaceSelection("    ") end
    if focused and (UserInputService:IsKeyDown(Enum.KeyCode.LeftControl) or UserInputService:IsKeyDown(Enum.KeyCode.RightControl)) then
        if input.KeyCode == Enum.KeyCode.Z then undoEdit(false)
        elseif input.KeyCode == Enum.KeyCode.Y then undoEdit(true) end
    end
end)

local renderTabs
local tabEditing = {last=nil, time=0}
function tabEditing.defaultName()
    local used = {}
    for _, tab in tabs do used[tab.name] = true end
    local number, name = 0, "New Script"
    while used[name] do number += 1; name = "New Script (" .. number .. ")" end
    return name
end
function tabEditing.finish(cancel)
    local input, tab = tabEditing.input, tabEditing.tab
    if not input then return end
    tabEditing.input, tabEditing.tab = nil, nil
    if not cancel and table.find(tabs, tab) then
        local name = input.Text:gsub("[%c]", " "):match("^%s*(.-)%s*$")
        local cutoff = utf8.offset(name, 41)
        if cutoff then name = name:sub(1, cutoff - 1) end
        if name ~= "" then tab.name = name end
    end
    input:ReleaseFocus()
    input:Destroy()
    if renderTabs then renderTabs() end
    persist()
end
function tabEditing.begin(tab, view)
    tabEditing.finish(false)
    local input = new("TextBox", "Rename", view.button, {
        BackgroundColor3=colors.accent, BackgroundTransparency=0, Text=tab.name,
        TextColor3=colors.ink, Font=Enum.Font.BuilderSansBold, TextSize=10,
        ClearTextOnFocus=false, ShowNativeInput=true, MultiLine=false,
        Size=UDim2.fromOffset(58, 31), Position=UDim2.fromOffset(34, 0), TextXAlignment=Enum.TextXAlignment.Left, ZIndex=12,
    })
    tabEditing.input, tabEditing.tab = input, tab
    observe(input.FocusLost, function() if tabEditing.input == input then tabEditing.finish(false) end end)
    input:CaptureFocus()
    input.CursorPosition, input.SelectionStart = #input.Text + 1, 1
end
local function selectScript(index)
    assert(tabs[index], "tab does not exist")
    tabEditing.finish(false)
    editor:ReleaseFocus()
    persist()
    activeScript = index
    trackedTab, trackedText, historyTime = tabs[index], tabs[index].text, -1
    lastCursor, lastSelection = #tabs[index].text + 1, -1
    editor.Text = tabs[index].text
    scroll.CanvasPosition = Vector2.new(0, 0)
    updateEditorHeight()
    renderTabs()
    persist()
end
local function addScript(text, name, savedId)
    if #tabs >= 24 then return false end -- 24 tabs (raised per community feedback)
    persist()
    createdTabs += 1
    tabs[#tabs + 1] = {name = name or tabEditing.defaultName(), savedId=savedId,
        text = text or "", path = ".basalt/tab-" .. createdTabs .. ".luau"}
    selectScript(#tabs)
    return true
end
local function closeScript(index)
    if not tabs[index] then return end
    tabEditing.finish(false)
    persist()
    table.remove(tabs, index)
    if #tabs == 0 then
        createdTabs += 1
        tabs[1] = {name="New Script", text="", path=".basalt/tab-" .. createdTabs .. ".luau"}
    end
    if index < activeScript then activeScript -= 1 end
    activeScript = math.clamp(activeScript, 1, #tabs)
    trackedTab, trackedText, historyTime = tabs[activeScript], tabs[activeScript].text, -1
    lastCursor, lastSelection = #trackedText + 1, -1
    editor.Text = tabs[activeScript].text
    updateEditorHeight()
    renderTabs()
    persist()
end
local tabConnections = {}
local tabViews = {}
local tabInfo = TweenInfo.new(0.18, Enum.EasingStyle.Quad, Enum.EasingDirection.Out)
local tabRevision = 0
local firstTabRender = true
local function settleTabAppearance()
    for tab, view in tabViews do
        for _, tween in view.fades or {} do tween:Cancel() end
        view.fades = {}
        local selected = tabs[activeScript] == tab
        view.button.BackgroundTransparency = 0
        view.caption.TextTransparency = selected and 0 or 0.12
        view.icon.ImageTransparency = selected and 0 or 0.12
        view.close.Icon.ImageTransparency = 0.15
    end
end
local function revealScript()
    local left = (activeScript - 1) * 120
    local right = left + 116
    if activeScript == #tabs then right = design(tabStrip, "CanvasSize").X.Offset end
    left, right = left * unitScale, right * unitScale
    local width = tabStrip.AbsoluteSize.X
    local x = tabStrip.CanvasPosition.X
    if left < x then x = left elseif right > x + width then x = right - width end
    tabStrip.CanvasPosition = Vector2.new(math.clamp(x, 0, math.max(0, tabStrip.CanvasSize.X.Offset - width)), 0)
end
renderTabs = function()
    tabRevision += 1
    local revision, removing, initial = tabRevision, false, firstTabRender
    for tab, view in tabViews do
        if not table.find(tabs, tab) then
            removing = true
            tabViews[tab] = nil
            view.button.Name = "ClosingTab"
            for _, connection in view.connections do connection:Disconnect() end
            local position = design(view.button, "Position")
            setUnit(view.button, "Position", UDim2.fromOffset(position.X.Offset, 7), TweenInfo.new(0.14))
            TweenService:Create(view.button, TweenInfo.new(0.14), {BackgroundTransparency=1}):Play()
            TweenService:Create(view.caption, TweenInfo.new(0.14), {TextTransparency=1}):Play()
            TweenService:Create(view.icon, TweenInfo.new(0.14), {ImageTransparency=1}):Play()
            TweenService:Create(view.close.Icon, TweenInfo.new(0.14), {ImageTransparency=1}):Play()
            task.delay(0.15, function() if view.button.Parent then view.button:Destroy() end end)
        end
    end
    local x = 0
    for index, tab in tabs do
        local width = 116
        local selected = index == activeScript
        local view = tabViews[tab]
        if not view then
            local control = surface(button("Tab" .. index, tabStrip, "", UDim2.fromOffset(width, 31),
                UDim2.fromOffset(x + 8, 0), colors.accent), 16)
            control.BackgroundTransparency = 1
            setUnit(control.Corner, "CornerRadius", UDim.new(0.5, 0))
            local symbol = icon(control, "document", 14, UDim2.fromOffset(10, 8))
            local caption = label("Caption", control, tab.name, UDim2.fromOffset(width - 56, 31), UDim2.fromOffset(34, 0), 10)
            caption.TextTruncate = Enum.TextTruncate.AtEnd
            caption.TextTransparency = 1
            symbol.ImageTransparency = 1
            local close = button("Close", control, "", UDim2.fromOffset(26, 31), UDim2.fromOffset(width - 28, 0), colors.panel)
            close.BackgroundTransparency = 1
            icon(close, "close", 11, UDim2.fromOffset(8, 10))
            close.Icon.ImageTransparency = 1
            view = {button=control, icon=symbol, caption=caption, close=close, connections={}}
            view.connections[1] = control.Activated:Connect(function()
                local current = table.find(tabs, tab)
                if not current then return end
                local double = tabEditing.last == tab and os.clock() - tabEditing.time < 0.34
                tabEditing.last, tabEditing.time = tab, os.clock()
                if double then tabEditing.last = nil; tabEditing.begin(tab, view)
                else selectScript(current) end
            end)
            view.connections[2] = close.Activated:Connect(function()
                local current = table.find(tabs, tab)
                if current then closeScript(current) end
            end)
            buttonMotion(control, view.connections)
            for _, connection in view.connections do tabConnections[#tabConnections + 1] = connection end
            tabViews[tab] = view
        end
        local control = view.button
        control.Name = "Tab" .. index
        control.BackgroundColor3 = selected and colors.accent or colors.selected
        view.icon.ImageColor3 = selected and colors.ink or colors.muted
        view.caption.TextColor3 = selected and colors.ink or colors.muted
        view.caption.Font = selected and Enum.Font.BuilderSansBold or Enum.Font.BuilderSansMedium
        view.caption.Text = tab.name
        view.close.Icon.ImageColor3 = selected and colors.ink or colors.muted
        setUnit(view.caption, "Size", UDim2.fromOffset(width - 56, 31))
        setUnit(view.close, "Position", UDim2.fromOffset(width - 28, 0))
        local target = UDim2.fromOffset(x, 0)
        local function settle()
            if tabRevision ~= revision or not control.Parent then return end
            for _, tween in view.fades or {} do tween:Cancel() end
            if initial then
                setUnit(control, "Position", target)
                control.BackgroundTransparency = 0
                view.caption.TextTransparency = selected and 0 or 0.12
                view.icon.ImageTransparency = selected and 0 or 0.12
                view.close.Icon.ImageTransparency = 0.15
                return
            end
            setUnit(control, "Position", target, tabInfo)
            view.fades = {
                TweenService:Create(control, tabInfo, {BackgroundTransparency=0}),
                TweenService:Create(view.caption, tabInfo, {TextTransparency=selected and 0 or 0.12}),
                TweenService:Create(view.icon, tabInfo, {ImageTransparency=selected and 0 or 0.12}),
                TweenService:Create(view.close.Icon, tabInfo, {ImageTransparency=0.15}),
            }
            for _, tween in view.fades do tween:Play() end
        end
        if removing then task.delay(0.08, settle) else settle() end
        x += width + 4
    end
    local plusTarget = UDim2.fromOffset(x + 2, 0)
    local function movePlus()
        if tabRevision ~= revision or not addButton.Parent then return end
        if initial then setUnit(addButton, "Position", plusTarget)
        else setUnit(addButton, "Position", plusTarget, tabInfo) end
    end
    if removing then task.delay(0.08, movePlus) else movePlus() end
    setUnit(tabStrip, "CanvasSize", UDim2.fromOffset(x + 34, 0))
    if initial then revealScript()
    else task.delay(0.2, function() if tabRevision == revision and tabStrip.Parent then revealScript() end end) end
    firstTabRender = false
end
renderTabs()
observe(addButton.Activated, function() addScript() end)

local selectedTab = "Code"
local layout
local navPages={"Code","Console","Saved","Settings"}
local function layoutDock()
    local count,selected=0,1
    for index,control in dockButtons do
        count+=1
        setUnit(control,"Position",UDim2.fromOffset(3+(count-1)*38,3))
        if navPages[index]==selectedTab then selected=count end
    end
    local width=count*38+2
    local left=-(width+46)/2
    setUnit(dock,"Size",UDim2.fromOffset(width,40))
    setUnit(closeButton,"Position",UDim2.new(0.5,left+20,0,0))
    setUnit(dock,"Position",UDim2.new(0.5,left+46,0,0))
    setUnit(navSelection,"Position",UDim2.new(0,20+(selected-1)*38,0.5,0))
end
local navTween
local navInfo = TweenInfo.new(0.3, Enum.EasingStyle.Quint, Enum.EasingDirection.Out)
local function selectTab(name)
    assert(name == "Code" or name == "Console" or name == "Saved" or name == "Settings", "expected Code, Console, Saved, or Settings")
    local before, navStart = selectedTab, navSelection.Position
    selectedTab = name
    local editing = name == "Code"
    mainGlass.Visible = editing
    editorCard.Visible = editing
    consoleCard.Visible = name == "Console"
    if chrome.saved then
        chrome.saved.page.Visible = name == "Saved"
        if name == "Saved" then chrome.saved.show() end
    end
    settings.card.Visible = name == "Settings"
    actions.Visible = editing
    consoleClear.Visible = name == "Console"
    consoleCopy.Visible = name == "Console"
    tabStrip.Visible = editing
    addButton.Visible = editing
    for index, control in dockButtons do
        TweenService:Create(control.Icon, navInfo, {ImageColor3=navPages[index] == name and colors.ink or colors.text}):Play()
        control.BackgroundTransparency = 1
    end
    if navTween then navTween:Cancel() end
    layoutDock()
    if not editing then editor:ReleaseFocus() end
    if not editing then tabEditing.finish(false) end
    if layout then layout() end
    if before ~= name and expanded and panel.Visible and not chrome.moving then
        local destination = navSelection.Position
        navSelection.Position = navStart
        navTween = TweenService:Create(navSelection, navInfo, {Position=destination})
        navTween:Play()
    end
end
observe(codeTab.Activated, function() selectTab("Code") end)
observe(consoleTab.Activated, function() selectTab("Console") end)
chrome.saved=createSavedScripts({new=new, rounded=rounded, button=button, label=label, icon=icon, setUnit=setUnit,
    colors=colors, observe=observe, panel=panel, buttonMotion=buttonMotion, http=HttpService,
    tweenService=TweenService, scale=function() return unitScale end,
    maxBytes=MAX_SCRIPT_BYTES, addScript=addScript, selectCode=function() selectTab("Code") end,
    current=function()
        persist()
        local tab=tabs[activeScript]
        return {name=tab.name, savedId=tab.savedId, code=editor.Text}
    end,
    link=function(id) tabs[activeScript].savedId=id; persist() end})
observe(dockButtons[3].Activated, function() selectTab("Saved") end)
observe(dockButtons[4].Activated, function() selectTab("Settings") end)
local followOutput = true
local filteredOutput = ""
local function consoleBottom()
    return math.max(0, consoleScroll.AbsoluteCanvasSize.Y - consoleScroll.AbsoluteWindowSize.Y)
end
local function scrollOutput()
    if consoleScroll.Parent and followOutput then
        consoleScroll.CanvasPosition = Vector2.new(0, consoleBottom())
    end
    latestButton.Visible = not followOutput and consoleBottom() > 20
end
local function measureOutput()
    if not output.Parent then return end
    local plain, formatted = {}, {}
    for line in (output.Text .. "\n"):gmatch("(.-)\n") do
        plain[#plain + 1] = line
        local color = line:match("^%[error%]") and "f0a19a" or line:match("^%[warn%]") and "e5c18d"
        formatted[#formatted + 1] = color and ('<font color="#' .. color .. '">' .. escape(line) .. '</font>') or escape(line)
    end
    filteredOutput = table.concat(plain, "\n")
    consoleDisplay.Text = table.concat(formatted, "\n")
    local width = math.max(20, consoleScroll.AbsoluteSize.X - 12 * unitScale)
    local bounds = TextService:GetTextSize(filteredOutput, output.TextSize, output.Font, Vector2.new(width, 1000000))
    local height = filteredOutput == "" and 0 or math.ceil(bounds.Y * output.LineHeight + 4 * unitScale)
    setUnit(output, "Size", UDim2.new(1, -12, 0, height / unitScale))
    setUnit(consoleDisplay, "Size", UDim2.new(1, -12, 0, height / unitScale))
    setUnit(consoleScroll, "CanvasSize", UDim2.fromOffset(0, height / unitScale + 4))
    task.defer(scrollOutput)
end
observe(consoleClear.Activated, function()
    output.Text = ""
    followOutput = true
    consoleScroll.CanvasPosition = Vector2.new(0, 0)
    measureOutput()
end)
observe(consoleCopy.Activated, function() pcall(setclipboard, filteredOutput) end)
observe(latestButton.Activated, function() followOutput = true; scrollOutput() end)
local function append(line)
    if not output.Parent then return end
    local updated = output.Text == "" and tostring(line) or output.Text .. "\n" .. tostring(line)
    if #updated > 24000 then
        local first = updated:find("\n", #updated - 20000, true)
        updated = first and updated:sub(first + 1) or updated:sub(-20000)
    end
    output.Text = updated
    task.defer(scrollOutput)
end
observe(consoleScroll:GetPropertyChangedSignal("AbsoluteCanvasSize"), scrollOutput)
observe(consoleScroll:GetPropertyChangedSignal("CanvasPosition"), function()
    followOutput = consoleBottom() - consoleScroll.CanvasPosition.Y < 20
    latestButton.Visible = not followOutput and consoleBottom() > 20
end)
local measureQueued = false
observe(output:GetPropertyChangedSignal("Text"), function()
    if measureQueued then return end
    measureQueued = true
    task.defer(function() measureQueued = false; measureOutput() end)
end)
observe(consoleScroll:GetPropertyChangedSignal("AbsoluteSize"), measureOutput)
local function receiveLog(message, messageType)
    local prefix = messageType == Enum.MessageType.MessageError and "[error] " or
        messageType == Enum.MessageType.MessageWarning and "[warn] " or ""
    append(prefix .. message)
end
local consoleBus = shared.__basaltConsole
if consoleBus then
    for _, entry in consoleBus.history do receiveLog(entry.message, entry.messageType) end
    subscriptions[#subscriptions + 1] = consoleBus.subscribe(receiveLog)
end
local execution = createExecutionService({maxBytes=MAX_SCRIPT_BYTES, compile=loadstring,
    print=print, warn=warn, reportError=function(message) _reportScriptError(message) end})
local function run(code, origin)
    if type(code) ~= "string" or not code:match("%S") then return false, "No code to run" end
    if preferences.clearConsoleOnExecute then
        output.Text = ""
        followOutput = true
        consoleScroll.CanvasPosition = Vector2.new(0, 0)
        measureOutput()
    end
    if origin ~= "clipboard" then editor.Text = code end
    local ok, job = pcall(execution.start, code, origin or "editor")
    if not ok then
        local problem = tostring(job)
        append("[error] " .. problem)
        return false, problem
    end
    if job.status == "compile_error" then return false, job.error end
    return true, job.id
end
observe(runButton.Activated, function() run(editor.Text) end)
do
local clearTab, clearUntil, clearRevision = nil, 0, 0
observe(clearButton.Activated, function()
    if clearTab == tabs[activeScript] and editor.Text == "" and os.clock() < clearUntil then
        undoEdit(false)
        clearTab = nil
        clearButton.Text = "Clear"
        return
    end
    if editor.Text == "" then return end
    setCode("", 1, true)
    clearTab, clearUntil = tabs[activeScript], os.clock() + 5
    clearButton.Text = "Undo"
    clearRevision += 1
    local revision = clearRevision
    task.delay(5, function()
        if revision == clearRevision and clearButton.Parent then
            clearTab = nil
            clearButton.Text = "Clear"
        end
    end)
end)
end
local function pasteClipboard()
    local ok, code = pcall(getclipboard)
    if not ok then
        local message = "Could not read clipboard: " .. tostring(code)
        append("[error] " .. message)
        return false, message
    end
    if type(code) ~= "string" or code == "" then
        append("[warn] Clipboard is empty")
        return false, "Clipboard is empty"
    end
    local pasted, problem = replaceSelection(code)
    if not pasted then append("[warn] " .. problem) end
    return pasted, problem
end
observe(clipboardButton.Activated, pasteClipboard)
local function executeClipboard()
    local ok, code = pcall(getclipboard)
    if not ok then
        local message = "Could not read clipboard: " .. tostring(code)
        append("[error] " .. message)
        return false, message
    end
    if type(code) ~= "string" or not code:match("%S") then
        append("[warn] Clipboard is empty")
        return false, "Clipboard is empty"
    end
    local started, result = run(code, "clipboard")
    return started, result
end
observe(executeClipboardButton.Activated, executeClipboard)

local blurStrength = preferences.backgroundBlur and 32 or 0

do
local frameTime, frameCount = 0, 0
local function updateStatus(fps)
    if fps then fpsText.Text = tostring(math.floor(fps + 0.5)) .. " FPS" end
    clockText.Text = os.date("%I:%M %p"):gsub("^0", "")
end
observe(RunService.RenderStepped, function(dt)
    if focused then
        caret.Visible = editor.CursorPosition > 0 and not nativeInput.selecting and (editor.SelectionStart < 1 or editor.SelectionStart == editor.CursorPosition) and
            (os.clock() - caretClock) % 1 < 0.6
    end
    frameTime += dt
    frameCount += 1
    if frameTime >= 1 then updateStatus(frameCount / frameTime); frameCount, frameTime = 0, 0 end
end)
updateStatus()
end

local menuVisible = false
local layoutOverride, keyboardOverride
layout = function()
    local area = layoutOverride or root.AbsoluteSize
    if area.X < 240 or area.Y < 200 then return end
    for _, entry in pillEntries do alignPill(entry) end
    local keyboard = UserInputService.OnScreenKeyboardVisible and
        math.clamp(UserInputService.OnScreenKeyboardSize.Y, 0, area.Y * 0.68) or 0
    if keyboardOverride ~= nil then keyboard = keyboardOverride end
    local usable = math.max(150, area.Y - keyboard)
    if keyboard > 0 and keyboardOverride == nil then
        local keyboardTop = UserInputService.OnScreenKeyboardPosition.Y
        local rootTop = root.AbsolutePosition.Y + screenOffset().Y
        if keyboardTop > rootTop then usable = math.max(150, math.min(usable, keyboardTop - rootTop)) end
    end
    local portrait = area.X < 560 and usable > 280
    local factor = (portrait and math.clamp(area.X / 390, 0.82, 1.2) or math.min(area.X / 704, (usable - 14) / 310, 2.6)) * (preferences.uiScale / 100)
    local width = portrait and (area.X - 24) / factor or math.max(536, math.min(area.X - 24, 360) / factor)
    local normalPortrait = area.X < 560 and area.Y > 280
    local normalFactor = (normalPortrait and math.clamp(area.X / 390, 0.82, 1.2) or math.min(area.X / 704, (area.Y - 14) / 310, 2.6)) * (preferences.uiScale / 100)
    codeFontScale = keyboard > 0 and normalFactor / factor or 1
    if keyboard > 0 then
        local normalWidth = normalPortrait and area.X - 24 or math.max(536 * normalFactor, math.min(area.X - 24, 360))
        width = normalWidth / factor
    end
    local height = portrait and math.min(620, (usable - 32) / factor) or 310
    scale.Scale = factor
    rescale(factor)
    if navTween then navTween:Cancel(); navTween=nil end
    panel.Size = UDim2.fromOffset(math.round(width * factor), math.round(height * factor))
    panel.Position = UDim2.fromOffset((area.X - width * factor) / 2, (usable - height * factor) / 2)
    layoutDock()
    local statusHeight = math.max(22, 22 / factor)
    local statusY = 40 + math.min(4, 8 / factor)
    local header = statusY + statusHeight + 5 / factor
    local pillX = 0
    for index, entry in pillEntries do
        local textSize, iconSize = math.max(10, 10 / factor), math.max(13, 13 / factor)
        setUnit(entry.text, "TextSize", textSize)
        setUnit(entry.icon, "Size", UDim2.fromOffset(iconSize, iconSize))
        local pillWidth = (index == 1 and 56 or index == 2 and 68 or 78) * math.max(1, 1 / factor)
        setUnit(entry.pill, "Size", UDim2.fromOffset(pillWidth, statusHeight))
        setUnit(entry.pill, "Position", UDim2.fromOffset(index == #pillEntries and width - pillWidth or pillX, statusY))
        if index < #pillEntries then pillX += pillWidth + 4 / factor end
    end
    local discordWidth = math.max(30, 30 / factor)
    setUnit(chrome.discord, "Position", UDim2.fromOffset(pillX, statusY))
    setUnit(chrome.discord, "Size", UDim2.fromOffset(discordWidth, statusHeight))
    setUnit(chrome.discord.Icon, "Position", UDim2.fromScale(0.5, 0.5))
    chrome.discord.Icon.AnchorPoint = Vector2.new(0.5, 0.5)
    setUnit(chrome.discord.Icon, "Size", UDim2.fromOffset(math.max(15, 15 / factor), math.max(15, 15 / factor)))
    setUnit(mainGlass, "Position", UDim2.fromOffset(0, header))
    setUnit(mainGlass, "Size", UDim2.fromOffset(width, height - header))
    setUnit(tabStrip, "Position", UDim2.fromOffset(12, header + 12))
    setUnit(tabStrip, "Size", UDim2.fromOffset(width - 24, 31))
    revealScript()
    local actionsHeight = 31
    setUnit(actions, "Position", UDim2.fromOffset(12, height - actionsHeight - 12))
    setUnit(actions, "Size", UDim2.fromOffset(width - 24, actionsHeight))
    local actionScale, actionOffset = math.min(1, (width - 24 - 16) / 410), 0
    for _, entry in {{runButton, 100}, {executeClipboardButton, 144}, {editActions, 166}} do
        local control, actionWidth = entry[1], entry[2] * actionScale
        setUnit(control, "Size", UDim2.fromOffset(actionWidth, actionsHeight))
        setUnit(control, "Position", UDim2.new(1, -actionOffset, 0, 0))
        actionOffset += actionWidth + 8
    end
    local half = 83 * actionScale
    setUnit(clearButton, "Size", UDim2.fromOffset(half, actionsHeight))
    setUnit(clipboardButton, "Size", UDim2.fromOffset(half, actionsHeight))
    setUnit(clipboardButton, "Position", UDim2.fromOffset(2 * half, 0))
    setUnit(editDivider, "Position", UDim2.fromOffset(half - 0.5, 7))
    setUnit(editorCard, "Position", UDim2.fromOffset(12, header + 51))
    setUnit(editorCard, "Size", UDim2.fromOffset(width - 24, height - header - 71 - actionsHeight))
    setUnit(consoleCard, "Position", UDim2.fromOffset(0, header))
    setUnit(consoleCard, "Size", UDim2.fromOffset(width, height - header))
    chrome.saved.layout(width, height, header)
    settings.layout(width, height, header)
    updateEditorHeight()
    measureOutput()
    for _, entry in pillEntries do alignPill(entry) end
    chrome.alignExecute()
    if not chrome.dragged then chrome.home = UDim2.new(0.5, -24, 0.17, -root.AbsolutePosition.Y) end
    local offset = chrome.home
    local x = offset.X.Scale * area.X + offset.X.Offset
    local y = offset.Y.Scale * area.Y + offset.Y.Offset
    if not chrome.moving then
        floating.Position = UDim2.fromOffset(math.clamp(x, 8, math.max(8, area.X - 56)), math.clamp(y, 8, math.max(8, area.Y - 56)))
    end
end
observe(root:GetPropertyChangedSignal("AbsoluteSize"), layout)
observe(UserInputService:GetPropertyChangedSignal("OnScreenKeyboardVisible"), layout)
observe(UserInputService:GetPropertyChangedSignal("OnScreenKeyboardSize"), layout)
observe(UserInputService:GetPropertyChangedSignal("OnScreenKeyboardPosition"), layout)

local dragging, dragStart, iconStart
local moved = false
local gestureId = 0
local finishedGestureId = -1
local function isPointerPress(input)
    return input and (input.UserInputType == Enum.UserInputType.Touch or
        input.UserInputType == Enum.UserInputType.MouseButton1)
end
local function matchesGesture(input)
    return dragging and (input == dragging or
        dragging.UserInputType == Enum.UserInputType.MouseButton1 and
        input and input.UserInputType == Enum.UserInputType.MouseButton1)
end
observe(floating.InputBegan, function(input)
    if not isPointerPress(input) or dragging or chrome.moving then return end
    gestureId += 1
    dragging = input
    moved = false
    dragStart = Vector2.new(input.Position.X, input.Position.Y)
    iconStart = floating.AbsolutePosition - root.AbsolutePosition
end)
observe(UserInputService.InputChanged, function(input)
    if not dragging then return end
    if dragging.UserInputType == Enum.UserInputType.Touch and input ~= dragging then return end
    if input.UserInputType ~= Enum.UserInputType.Touch and input.UserInputType ~= Enum.UserInputType.MouseMovement then return end
    local delta = Vector2.new(input.Position.X, input.Position.Y) - dragStart
    if delta.Magnitude < 6 and not moved then return end
    moved = true
    local area = root.AbsoluteSize
    local position = iconStart + delta
    floating.Position = UDim2.fromOffset(
        math.clamp(position.X, 8, math.max(8, area.X - floating.AbsoluteSize.X - 8)),
        math.clamp(position.Y, 8, math.max(8, area.Y - floating.AbsoluteSize.Y - 8)))
end)

local fadeDuration = 0.2
local fadeInfo = TweenInfo.new(fadeDuration, Enum.EasingStyle.Quad, Enum.EasingDirection.InOut)
local panelFade, backdropFade, blurFade, opacityFade
observe(workspace:GetPropertyChangedSignal("CurrentCamera"), function()
    if workspace.CurrentCamera and blur.Parent == workspace.CurrentCamera then return end
    if blurFade then blurFade:Cancel(); blurFade = nil end
    blur:Destroy()
    blur = new("BlurEffect", "BlurEffect", workspace.CurrentCamera, {Size=expanded and panel.Visible and blurStrength or 0, Enabled=true})
    if diagnostics and shared.basaltEditor and shared.basaltEditor.gui == gui then shared.basaltEditor.blur = blur end
end)
local opacity = new("NumberValue", "Opacity", panel, {Value = 0})
local fadeEntries = {}
local function applyOpacity(amount)
    for _, entry in fadeEntries do
        if entry.object.Parent then
            for property, value in entry.values do entry.object[property] = value + (1 - value) * amount end
        end
    end
end
local function restoreOpacity()
    applyOpacity(0)
    table.clear(fadeEntries)
    opacity.Value = 0
end
local function captureOpacity()
    for _, object in panel:GetDescendants() do
        local values = {}
        if object:IsA("GuiObject") then
            values.BackgroundTransparency = object.BackgroundTransparency
            if object:IsA("TextLabel") or object:IsA("TextButton") or object:IsA("TextBox") then
                values.TextTransparency = object.TextTransparency
            elseif object:IsA("ImageLabel") or object:IsA("ImageButton") then
                values.ImageTransparency = object.ImageTransparency
            end
        elseif object:IsA("UIStroke") then
            values.Transparency = object.Transparency
        end
        if next(values) then fadeEntries[#fadeEntries + 1] = {object=object, values=values} end
    end
end
observe(opacity.Changed, applyOpacity)
local transition = 0
local function cancelFades()
    if panelFade then panelFade:Cancel(); panelFade = nil end
    if backdropFade then backdropFade:Cancel(); backdropFade = nil end
    if blurFade then blurFade:Cancel(); blurFade = nil end
    if opacityFade then opacityFade:Cancel(); opacityFade = nil end
    for _, tween in chrome.tweens do tween:Cancel() end
    table.clear(chrome.tweens)
    restoreOpacity()
end
function chrome.morph(opening)
    local destination = closeButton.AbsolutePosition - root.AbsolutePosition
    local size = closeButton.AbsoluteSize
    if not chrome.moving then
        if opening then
            floating.Position, floating.Size = chrome.home, UDim2.fromOffset(48, 48)
            floating.Logo.ImageTransparency, chrome.close.ImageTransparency = 0, 1
        else
            floating.Position, floating.Size = UDim2.fromOffset(destination.X, destination.Y), UDim2.fromOffset(size.X, size.Y)
            floating.Logo.ImageTransparency, chrome.close.ImageTransparency = 1, 0
        end
    end
    chrome.moving = true
    floating.Visible, closeButton.Visible = true, false
    chrome.tweens = {
        TweenService:Create(floating, fadeInfo, {Position=opening and UDim2.fromOffset(destination.X, destination.Y) or chrome.home,
            Size=opening and UDim2.fromOffset(size.X, size.Y) or UDim2.fromOffset(48, 48)}),
        TweenService:Create(floating.Logo, fadeInfo, {ImageTransparency=opening and 1 or 0}),
        TweenService:Create(chrome.close, fadeInfo, {ImageTransparency=opening and 0 or 1}),
    }
    for _, tween in chrome.tweens do tween:Play() end
end
local function animate(opening)
    transition += 1
    local current = transition
    cancelFades()
    settleTabAppearance()
    resetBrand(not opening)
    captureOpacity()
    if opening then
        layout()
        panel.Visible = true
        backdrop.Visible = true
        backdrop.BackgroundTransparency = 1
        opacity.Value = 1
        applyOpacity(1)
        opacityFade = TweenService:Create(opacity, fadeInfo, {Value = 0})
        task.delay(fadeDuration, function()
            if transition == current and panel.Parent then
                applyOpacity(0); table.clear(fadeEntries)
                chrome.moving = false
                floating.Visible, closeButton.Visible = false, true
                floating.Position, floating.Size = chrome.home, UDim2.fromOffset(48, 48)
            end
        end)
        local destination = panel.Position
        panelFade = TweenService:Create(panel, fadeInfo, {Position = destination})
        backdropFade = TweenService:Create(backdrop, fadeInfo, {BackgroundTransparency = 0.78})
    else
        opacityFade = TweenService:Create(opacity, fadeInfo, {Value = 1})
        panelFade = TweenService:Create(panel, fadeInfo, {Position = panel.Position})
        backdropFade = TweenService:Create(backdrop, fadeInfo, {BackgroundTransparency = 1})
        task.delay(fadeDuration, function()
            if transition ~= current or not panel.Parent then return end
            panel.Visible = false
            backdrop.Visible = false
            floating.Visible = true
            closeButton.Visible = true
            chrome.moving = false
            restoreOpacity()
        end)
    end
    chrome.morph(opening)
    blurFade = TweenService:Create(blur, fadeInfo, {Size = opening and blurStrength or 0})
    panelFade:Play()
    backdropFade:Play()
    blurFade:Play()
    opacityFade:Play()
end
local function open()
    expanded = true
    layoutOverride, keyboardOverride = nil, nil
    animate(true)
end
settings.changed = function(values, before)
    preferences = values
    if values.uiScale ~= before.uiScale then layout() end
    lineNumbers.Visible = values.lineNumbers
    if values.textSize ~= before.textSize or values.syntaxHighlighting ~= before.syntaxHighlighting then
        local wasFocused = editor:IsFocused()
        if wasFocused then editor:ReleaseFocus() end
        local line = scroll.CanvasPosition.Y / (editor.TextSize * editor.LineHeight)
        codeFontSize = values.textSize
        updateEditorHeight()
        scroll.CanvasPosition = Vector2.new(scroll.CanvasPosition.X, line * editor.TextSize * editor.LineHeight)
        if wasFocused then editor:CaptureFocus() end
    end
    if values.backgroundBlur ~= before.backgroundBlur then
        blurStrength = values.backgroundBlur and 32 or 0
        if blurFade then blurFade:Cancel() end
        blurFade = TweenService:Create(blur, TweenInfo.new(0.18, Enum.EasingStyle.Quad, Enum.EasingDirection.Out),
            {Size=expanded and panel.Visible and blurStrength or 0})
        blurFade:Play()
    end
end
local function dismiss()
    if not expanded then return end
    expanded = false
    editor:ReleaseFocus()
    nativeInput.sync()
    tabEditing.finish(false)
    animate(false)
    persist()
end
local function finishGesture(input, activated)
    if not matchesGesture(input) then return end
    local current = gestureId
    local wasDrag = moved
    local inside = activated
    if not inside and input.Position then
        local p, size = floating.AbsolutePosition, floating.AbsoluteSize
        inside = input.Position.X >= p.X and input.Position.X <= p.X + size.X and
            input.Position.Y >= p.Y and input.Position.Y <= p.Y + size.Y
    end
    -- Clear pointer state before opening; Activated may precede InputEnded.
    dragging = nil
    moved = false
    finishedGestureId = current
    if wasDrag then chrome.home, chrome.dragged = floating.Position, true end
    if not wasDrag and inside and floating.Visible then open() end
end
observe(floating.Activated, function(input)
    if matchesGesture(input) then
        finishGesture(input, true)
    elseif not isPointerPress(input) and floating.Visible then
        open()
    end
end)
observe(UserInputService.InputEnded, function(input)
    if not matchesGesture(input) then return end
    local current = gestureId
    -- Let the button activation run first when the engine emits it on release.
    task.defer(function()
        if current == gestureId and current ~= finishedGestureId then
            finishGesture(input, false)
        end
    end)
end)
observe(backdrop.Activated, dismiss)
observe(closeButton.Activated, dismiss)
observe(UserInputService.InputBegan, function(input)
    if not expanded or menuVisible or not panel.Visible or not isPointerPress(input) then return end
    local point = Vector2.new(input.Position.X, input.Position.Y)
    for _, control in {mainGlass, consoleCard, chrome.saved.page, settings.card, closeButton, dock, fpsPill, clockPill, chrome.discord} do
        local position, size = control.AbsolutePosition, control.AbsoluteSize
        if control.Visible and point.X >= position.X and point.Y >= position.Y and
            point.X <= position.X + size.X and point.Y <= position.Y + size.Y then return end
    end
    dismiss()
end)
observe(GuiService.MenuOpened, function()
    menuVisible = true
    backdrop.Visible = false
    editor:ReleaseFocus()
    tabEditing.finish(false)
    persist()
end)
observe(GuiService.MenuClosed, function()
    menuVisible = false
    if expanded and panel.Visible then backdrop.Visible = true end
end)

local editorState
editorState = {
    version = VERSION,
    gui = gui,
    dimGui = dimGui,
    source = editor,
    console = output,
    saved = chrome.saved,
    settings = settings,
    execution = execution,
    openDiscord = openDiscordInvite,
    editorInput = editorInputBridge,
    protectionControl = serviceProtectionBridge,
    getNativeInputState = function() return nativeInput.state end,
    getScreenOffset = screenOffset,
    run = run,
    pasteClipboard = pasteClipboard,
    executeClipboard = executeClipboard,
    open = open,
    close = dismiss,
    selectTab = selectTab,
    persist = persist,
    tabs = tabs,
    getActiveScript = function() return activeScript end,
    getDesignScale = function() return unitScale end,
    selectScript = selectScript,
    addScript = addScript,
    closeScript = closeScript,
    refresh = updateEditorHeight,
    edit = setCode,
    indent = function() replaceSelection("    ") end,
    undo = function() undoEdit(false) end,
    redo = function() undoEdit(true) end,
    consoleScroll = consoleScroll,
    followConsole = function() followOutput = true; scrollOutput() end,
    blur = blur,
    opacity = opacity,
    testLayout = function(size, keyboard)
        layoutOverride, keyboardOverride = size, keyboard
        layout()
    end,
    destroy = function()
        transition += 1
        cancelFades()
        settings.destroy()
        tabEditing.finish(false)
        editor:ReleaseFocus()
        expanded = false
        nativeInput.sync()
        persist()
        for _, connection in subscriptions do connection:Disconnect() end
        for _, connection in tabConnections do connection:Disconnect() end
        blur:Destroy()
        gui:Destroy()
        dimGui:Destroy()
        if diagnostics and shared.basaltEditor == editorState then shared.basaltEditor = nil end
    end,
}
if diagnostics then shared.basaltEditor = editorState end
selectTab("Code")
expanded = false
panel.Visible, backdrop.Visible, floating.Visible = false, false, true
resetBrand(false)
layout()
