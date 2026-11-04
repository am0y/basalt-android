-- Saved scripts share the existing editor panel and autoexec folder.
local new, rounded, button, label, icon = context.new, context.rounded, context.button, context.label, context.icon
local setUnit, colors, observe = context.setUnit, context.colors, context.observe
local TweenService=context.tweenService
local directory, autoDirectory, indexPath = "basalt/scripts", "basalt/autoexec", ".basalt/saved-scripts.json"
local items, rows, serial = {}, {}, 0
local page = rounded(new("Frame", "SavedScripts", context.panel, {
    BackgroundColor3=colors.panel, BackgroundTransparency=0.08,
    Position=UDim2.fromOffset(0, 77), Size=UDim2.fromOffset(536, 233),
    ClipsDescendants=true, Visible=false, ZIndex=5,
}), 17)
local save=button("SaveCurrent", page, "", UDim2.fromOffset(30, 30), UDim2.new(1, -44, 0, 9), colors.accent, colors.ink)
setUnit(save.Corner, "CornerRadius", UDim.new(1, 0))
icon(save, "add", 16, UDim2.fromScale(0.5, 0.5), colors.ink).AnchorPoint=Vector2.new(0.5, 0.5)
local list=new("ScrollingFrame", "Scripts", page, {
    BackgroundTransparency=1, Position=UDim2.fromOffset(12, 48), Size=UDim2.new(1, -24, 1, -60),
    CanvasSize=UDim2.fromOffset(0, 0), ScrollBarThickness=2, ScrollBarImageColor3=colors.muted,
    ScrollingDirection=Enum.ScrollingDirection.Y, ZIndex=6,
})
local saved={page=page, items=items, list=list}
local function sourcePath(id) return directory .. "/" .. id .. ".luau" end
local function autoPath(id) return autoDirectory .. "/basalt-saved-" .. id .. ".luau" end
local function ensureFolders()
    if not isfolder("basalt") then makefolder("basalt") end
    if not isfolder(directory) then makefolder(directory) end
    if not isfolder(autoDirectory) then makefolder(autoDirectory) end
end
local function find(id)
    for _, item in items do if item.id == id then return item end end
end
local function name(value)
    if type(value) ~= "string" or not utf8.len(value) then return "Script" end
    local clean=value:gsub("[%c]", " "):match("^%s*(.-)%s*$")
    local cutoff=utf8.offset(clean, 41)
    return clean ~= "" and (cutoff and clean:sub(1, cutoff - 1) or clean) or "Script"
end
local function persist()
    local metadata={}
    for _, item in items do metadata[#metadata+1]={id=item.id, name=item.name} end
    writefile(indexPath, context.http:JSONEncode({version=1, scripts=metadata}))
end
pcall(function()
    local data=readfile(indexPath)
    if #data>32768 then return end
    local decoded=context.http:JSONDecode(data)
    if decoded.version~=1 or type(decoded.scripts)~="table" then return end
    for _, item in decoded.scripts do
        if #items>=80 then break end
        if type(item)=="table" and type(item.id)=="string" and #item.id<=48 and
            item.id:match("^[%w%-]+$") and not find(item.id) and isfile(sourcePath(item.id)) then
            items[#items+1]={id=item.id, name=name(item.name)}
        end
    end
end)
local function report(problem) warn("[Basalt Saved Scripts] " .. tostring(problem)) end
local function clearRows()
    for _, row in rows do row:Destroy() end
    table.clear(rows)
end
local render
render=function()
    clearRows()
    for index, item in items do
        local row=new("Frame", "Saved" .. index, list, {
            BackgroundTransparency=1, Size=UDim2.new(1, -3, 0, 48),
            Position=UDim2.fromOffset(0, (index-1)*54), ZIndex=7,
        })
        rows[#rows+1]=row
        new("Frame", "Divider", row, {BackgroundColor3=colors.edge, BackgroundTransparency=0.8,
            Position=UDim2.new(0, 10, 1, -1), Size=UDim2.new(1, -20, 0, 1), ZIndex=8})
        local title=label("Title", row, item.name, UDim2.new(1, -166, 0, 20), UDim2.fromOffset(11, 4), 11)
        title.Font, title.TextTruncate=Enum.Font.BuilderSansBold, Enum.TextTruncate.AtEnd
        local enabled=isfile(autoPath(item.id))
        local detail=label("Mode", row, enabled and "Autoexec on" or "Autoexec off", UDim2.new(1, -166, 0, 15),
            UDim2.fromOffset(11, 25), 9, colors.muted)
        detail.TextTruncate=Enum.TextTruncate.AtEnd
        local toggle=button("Autoexec", row, "", UDim2.fromOffset(44, 24), UDim2.new(1, -112, 0.5, -12),
            enabled and colors.accent or colors.panel)
        setUnit(toggle.Corner, "CornerRadius", UDim.new(1, 0))
        local knob=rounded(new("Frame", "Knob", toggle, {
            BackgroundColor3=enabled and colors.ink or colors.muted, Size=UDim2.fromOffset(16, 16),
            Position=UDim2.fromOffset(enabled and 24 or 4, 4), ZIndex=toggle.ZIndex+1,
        }), 8)
        knob.Active=false
        local toggleRevision=0
        local open=button("Open", row, "", UDim2.fromOffset(28, 28), UDim2.new(1, -62, 0.5, -14), colors.selected)
        local remove=button("Delete", row, "", UDim2.fromOffset(28, 28), UDim2.new(1, -32, 0.5, -14), colors.selected)
        for control, glyph in {[open]="right", [remove]="trash"} do
            setUnit(control.Corner, "CornerRadius", UDim.new(1, 0))
            icon(control, glyph, 14, UDim2.fromScale(0.5, 0.5)).AnchorPoint=Vector2.new(0.5, 0.5)
        end
        toggle.Activated:Connect(function()
            local ok, problem=pcall(function()
                if isfile(autoPath(item.id)) then delfile(autoPath(item.id))
                else
                    ensureFolders()
                    writefile(autoPath(item.id), readfile(sourcePath(item.id)))
                end
            end)
            if not ok then report(problem); return end
            enabled=not enabled
            toggleRevision+=1
            local revision=toggleRevision
            detail.Text=enabled and "Autoexec on" or "Autoexec off"
            local ease=TweenInfo.new(0.18, Enum.EasingStyle.Quad, Enum.EasingDirection.Out)
            TweenService:Create(toggle, ease, {BackgroundColor3=enabled and colors.accent or colors.panel}):Play()
            local x=enabled and 24 or 4
            local factor=context.scale()
            local tween=TweenService:Create(knob, ease, {BackgroundColor3=enabled and colors.ink or colors.muted,
                Position=UDim2.fromOffset(math.round(x*factor), math.round(4*factor))})
            tween.Completed:Connect(function()
                if row.Parent and revision==toggleRevision then setUnit(knob,"Position",UDim2.fromOffset(x,4)) end
            end)
            tween:Play()
        end)
        open.Activated:Connect(function()
            local ok, code=pcall(readfile, sourcePath(item.id))
            if not ok or type(code)~="string" or #code>context.maxBytes then report("Unable to open script"); return end
            if context.addScript(code, item.name, item.id) then context.selectCode()
            else report("Close a tab first (24 max)") end
        end)
        remove.Activated:Connect(function()
            local ok, problem=pcall(function()
                delfile(sourcePath(item.id))
                if isfile(autoPath(item.id)) then delfile(autoPath(item.id)) end
                table.remove(items, assert(table.find(items, item)))
                persist()
            end)
            if not ok then report(problem) end
            render()
        end)
    end
    setUnit(list, "CanvasSize", UDim2.fromOffset(0, #items*54))
end
function saved.saveCurrent()
    local current=context.current()
    local code=current.code
    if type(code)~="string" or #code>context.maxBytes then report("Script is too large to save"); return false end
    local item=find(current.savedId)
    local fresh=not item
    if fresh and #items>=80 then report("Library is full"); return false end
    if fresh then
        repeat
            serial+=1
            local id="script-" .. tostring(os.time()) .. "-" .. tostring(serial)
            if not find(id) and not isfile(sourcePath(id)) then item={id=id, name=name(current.name)}; break end
        until serial>100000
    end
    if not item then report("Unable to save script"); return false end
    local ok, problem=pcall(function()
        ensureFolders()
        writefile(sourcePath(item.id), code)
        if isfile(autoPath(item.id)) then writefile(autoPath(item.id), code) end
        if fresh then items[#items+1]=item end
        item.name=name(current.name)
        persist()
    end)
    if not ok then report(problem); return false end
    context.link(item.id)
    render()
    return true
end
function saved.show() render() end
function saved.layout(width, height, header)
    setUnit(page, "Position", UDim2.fromOffset(0, header))
    setUnit(page, "Size", UDim2.fromOffset(width, height-header))
end
observe(save.Activated, saved.saveCurrent)
context.buttonMotion(save)
return saved
