local response = request({
    Url="http://10.0.2.2:17983/redirect-external",
    Headers={Authorization="Bearer disposable-test-value", Cookie="session=disposable-test-value"},
})
assert(response.Success and response.StatusCode == 200, "cross-origin redirect failed: " .. tostring(response.StatusCode))
local sent = game:GetService("HttpService"):JSONDecode(response.Body)
assert(not sent.authorization and not sent.cookie and not sent.proxy, "credentials leaked across a redirect")
assert(sent.host == "10.0.2.2:17984", "redirect target host was not normalized")
print("SECURITY_REDIRECT_OK", response.StatusCode)
