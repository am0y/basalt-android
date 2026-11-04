local core = game:GetService("CoreGui")
local interface
for _, item in core:GetDescendants() do
    if item.Name == "Interface" and item:IsA("ScreenGui") and item.Enabled and
        item.Parent and item.Parent.Name == "UI" and #item:GetDescendants() > 20 then
        interface = item
        break
    end
end
assert(interface, "Basalt editor Interface is missing or disabled")
local backdrop = interface.Parent:FindFirstChild("Backdrop")
assert(backdrop and backdrop:IsA("ScreenGui"), "Basalt editor backdrop is missing")
print("editor UI verified", interface:GetFullName(), #interface:GetDescendants())
