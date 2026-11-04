local targets = {
    HttpRbxApiService={"PostAsync", "PostAsyncFullUrl", "GetAsync", "GetAsyncFullUrl", "RequestAsync"},
    ScriptContext={"AddCoreScriptLocal"},
    BrowserService={"EmitHybridEvent", "ExecuteJavaScript", "OpenBrowserWindow", "OpenNativeOverlay", "ReturnToJavaScript", "SendCommand"},
    MarketplaceService={"GetRobuxBalance", "PerformPurchase", "PerformPurchaseV2", "PromptBundlePurchase", "PromptGamePassPurchase", "PromptProductPurchase", "PromptPurchase", "PromptRobloxPurchase", "PromptThirdPartyPurchase"},
    HttpService={"RequestInternal"}, GuiService={"OpenBrowserWindow", "OpenNativeOverlay"},
    OpenCloudService={"HttpRequestAsync"}, CoreGui={"TakeScreenshot", "ToggleRecording"},
    MessageBusService={"Call", "GetLast", "GetMessageId", "GetProtocolMethodRequestMessageId", "GetProtocolMethodResponseMessageId", "MakeRequest", "Publish", "PublishProtocolMethodRequest", "PublishProtocolMethodResponse", "Subscribe", "SubscribeToProtocolMethodRequest", "SubscribeToProtocolMethodResponse"},
    DataModel={"Load", "OpenScreenshotsFolder", "OpenVideosFolder", "HttpPost", "HttpPostAsync", "HttpGet", "HttpGetAsync"},
    OmniRecommendationsService={"MakeRequest"}, Players={"ReportAbuse", "ReportAbuseV3"},
}
local original = getthreadidentity()
local passed, absent, failures = 0, 0, {}
for class, names in targets do
    setthreadidentity(8)
    local service = class == "DataModel" and game or game:GetService(class)
    for _, name in names do
        local exists, direct = pcall(function() return service[name] end)
        if not exists or type(direct) ~= "function" then
            absent += 1
            print("SECURITY_UNAVAILABLE", class, name)
        else
            local colon = assert(loadstring("return function(self) return self:" .. name .. "() end"))()
            local cloned = clonefunction(direct)
            for _, identity in {2, 7, 8} do
                setthreadidentity(identity)
                local routes = {
                    namecall=function() return colon(service) end,
                    dot=function() return direct(service) end,
                    cloned=function() return cloned(service) end,
                }
                for route, call in routes do
                    local ok, problem = pcall(call)
                    if not ok and tostring(problem):find("Basalt security:", 1, true) then passed += 1
                    elseif (name == "HttpGet" or name == "HttpGetAsync") and not ok and tostring(problem):find("HTTP ", 1, true) then
                        passed += 1
                    else failures[#failures + 1] = class .. "." .. name .. " " .. route .. " identity=" .. identity .. " " .. tostring(problem) end
                end
            end
        end
    end
end
setthreadidentity(original)
for _, failure in failures do warn("SECURITY_FAILURE", failure) end
assert(#failures == 0, tostring(#failures) .. " native service protections failed")
assert(passed >= 350, "too few live security cases executed")
assert(_probeMemoryHex == nil and _probeObjectLayout == nil, "release exposes arbitrary process-memory readers")
assert(_openDiscordInvite == nil, "editor-only Discord opener leaked into executor scripts")
assert(_serviceProtection == nil, "editor-only protection control leaked into executor globals")
print("SECURITY_SERVICES_OK", passed, "blocked calls", absent, "unavailable methods")
