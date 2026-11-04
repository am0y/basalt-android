local core = game:GetService("CoreGui")
local gui = Instance.new("ScreenGui")
gui.Name = "Basalt744Smoke"
gui.ResetOnSpawn = false
gui.Parent = core
task.wait(0.1)
assert(gui.Parent == core, "CoreGui parenting did not persist")
gui:Destroy()
