local MOD_NAME = "RTX 40 MFG Unlock"
local CONFIG_PATH = "config.json"
local STATUS_PATH = "bridge_status.json"
local VALID_MULTIPLIERS = {
    [2] = true, [3] = true, [4] = true, [5] = true, [6] = true
}

local overlayOpen = false
local selectedMode = "fixed"
local selectedMultiplier = 2
local dynamicTargetFrameRate = 0
local dynamicExperimental56 = false
local generatedOnlyDebug = false
local lastCustomTarget = 120
local nativeStatusDetected = false
local nativeStatusVersion = nil
local liveBridgeDetected = false
local synthesisFallbackActive = false
local activeMode = nil
local activeMultiplier = nil
local activeDynamicTarget = nil
local activeDynamicExperimental56 = nil
local activeGeneratedOnlyDebug = nil
local activePatchRoute = nil
local appliedMode = nil
local appliedMultiplier = nil
local appliedDynamicTarget = nil
local requestPending = false
local streamlineRebuildRequired = false
local gameFrameGenerationOn = false
local setOptionsSeen = false
local setOptionsAccepted = false
local setOptionsResult = nil
local getStateSeen = false
local getStateResult = nil
local actualFramesPresented = nil
local stateSampleAgeMs = nil
local uiTagHookInstalled = false
local hudlessTagActive = false
local uiAlphaTagActive = false
local uiColorAlphaTagActive = false
local uiDimensionsKnown = false
local uiDimensionsMatch = false
local uiRecompositionEnabled = false
local uiRecompositionForced = false
local gameUiRecompositionEnabled = false
local realFps = nil
local dlssFps = nil
local fpsSampleAgeMs = nil
local statusMessage = ""
local bridgePollElapsed = 0

local function readBridgeState()
    local ok, state = pcall(function()
        local file = io.open(STATUS_PATH, "r")
        if not file then
            return nil
        end
        local content = file:read("*all")
        file:close()
        local decoded, data = pcall(json.decode, content)
        if not decoded or type(data) ~= "table" then
            return nil
        end
        local heartbeat = tonumber(data.heartbeat)
        if not heartbeat or math.abs(os.time() - heartbeat) > 4 then
            return nil
        end

        local multiplier = tonumber(data.multiplier)
        local mode = data.mode
        local target = tonumber(data.dynamicTargetFrameRate)
        local route = data.route

        if not VALID_MULTIPLIERS[multiplier] then
            return nil
        end
        if mode ~= "fixed" and mode ~= "dynamic" then
            mode = "fixed"
        end
        local statusVersion = tonumber(data.version) or 1
        return {
            bridgeReady = statusVersion >= 7 and data.bridgeReady == true,
            synthesisFallbackActive = data.synthesisFallbackActive == true,
            multiplier = multiplier,
            mode = mode,
            dynamicTargetFrameRate = target or 0,
            dynamicExperimental56 = data.dynamicExperimental56 == true,
            generatedOnlyDebug = data.generatedOnlyDebug == true,
            patchRoute = route,
            appliedMode = data.appliedMode or mode,
            appliedMultiplier = tonumber(data.appliedMultiplier) or multiplier,
            appliedDynamicTargetFrameRate = tonumber(data.appliedDynamicTargetFrameRate) or (target or 0),
            pending = data.pending == true,
            streamlineRebuildRequired = data.streamlineRebuildRequired == true,
            gameFrameGenerationOn = data.gameFrameGenerationOn == true,
            setOptionsSeen = data.setOptionsSeen == true,
            setOptionsAccepted = data.setOptionsAccepted == true
                or tonumber(data.setOptionsResult) == 0
                or tonumber(data.setOptionsResult) == 39,
            setOptionsResult = tonumber(data.setOptionsResult),
            getStateSeen = data.getStateSeen == true,
            getStateResult = tonumber(data.getStateResult),
            actualFramesPresented = tonumber(data.actualFramesPresented),
            stateSampleAgeMs = tonumber(data.stateSampleAgeMs),
            uiTagHookInstalled = data.uiTagHookInstalled == true,
            hudlessTagActive = data.hudlessTagActive == true,
            uiAlphaTagActive = data.uiAlphaTagActive == true,
            uiColorAlphaTagActive = data.uiColorAlphaTagActive == true,
            uiDimensionsKnown = data.uiDimensionsKnown == true,
            uiDimensionsMatch = data.uiDimensionsMatch == true,
            uiRecompositionEnabled = data.uiRecompositionEnabled == true,
            uiRecompositionForced = data.uiRecompositionForced == true,
            gameUiRecompositionEnabled = data.gameUiRecompositionEnabled == true,
            realFps = tonumber(data.realFpsMilli) and tonumber(data.realFpsMilli) / 1000 or nil,
            dlssFps = tonumber(data.dlssFpsMilli) and tonumber(data.dlssFpsMilli) / 1000 or nil,
            fpsSampleAgeMs = tonumber(data.fpsSampleAgeMs),
            statusVersion = statusVersion
        }
    end)
    return ok and state or nil
end

local function loadConfig()
    local file = io.open(CONFIG_PATH, "r")
    if not file then
        statusMessage = "Config missing; using fixed 2x until it can be saved."
        return
    end

    local content = file:read("*all")
    file:close()
    local ok, data = pcall(json.decode, content)
    if ok and type(data) == "table" then
        local multiplier = tonumber(data.multiplier)
        local mode = data.mode or "fixed"
        local target = tonumber(data.dynamicTargetFrameRate) or 0
        if VALID_MULTIPLIERS[multiplier]
            and (mode == "fixed" or mode == "dynamic")
            and target >= 0 and target <= 1000 then
            selectedMultiplier = multiplier
            selectedMode = mode
            dynamicTargetFrameRate = math.floor(target)
            dynamicExperimental56 = data.dynamicExperimental56 == true
            generatedOnlyDebug = data.generatedOnlyDebug == true
            if dynamicTargetFrameRate > 0 then
                lastCustomTarget = dynamicTargetFrameRate
            end
            return
        end
    end
    statusMessage = "Invalid config; choose a fixed multiplier or Dynamic to repair it."
end

local function describeRequest()
    if selectedMode == "dynamic" then
        if dynamicTargetFrameRate == 0 then
            return "Dynamic targeting the current display refresh rate"
        end
        return "Dynamic targeting " .. tostring(dynamicTargetFrameRate) .. " FPS"
    end
    return "Fixed " .. tostring(selectedMultiplier) .. "x"
end

local function saveConfig()
    local file = io.open(CONFIG_PATH, "w")
    if not file then
        statusMessage = "Could not write config.json. Check folder permissions."
        return false
    end

    local ok, encoded = pcall(json.encode, {
        mode = selectedMode,
        multiplier = selectedMultiplier,
        dynamicTargetFrameRate = dynamicTargetFrameRate,
        dynamicExperimental56 = dynamicExperimental56,
        generatedOnlyDebug = generatedOnlyDebug,
        version = 7
    })
    if not ok then
        file:close()
        statusMessage = "Could not encode config.json."
        return false
    end

    file:write(encoded)
    file:write("\n")
    file:close()
    if liveBridgeDetected then
        statusMessage = describeRequest() .. " requested."
    elseif nativeStatusDetected then
        statusMessage = describeRequest() .. " saved. Waiting for active DLSS-G modules."
    else
        statusMessage = describeRequest() .. " saved. Auto-loader not detected; verify bin/x64/plugins/RTX40MFG.asi."
    end
    print(MOD_NAME .. ": " .. statusMessage)
    return true
end

local function refreshBridgeStatus()
    local state = readBridgeState()
    if not state then
        nativeStatusDetected = false
        nativeStatusVersion = nil
        liveBridgeDetected = false
        synthesisFallbackActive = false
        activeMode = nil
        activeMultiplier = nil
        activeDynamicTarget = nil
        activeDynamicExperimental56 = nil
        activeGeneratedOnlyDebug = nil
        activePatchRoute = nil
        appliedMode = nil
        appliedMultiplier = nil
        appliedDynamicTarget = nil
        requestPending = false
        streamlineRebuildRequired = false
        gameFrameGenerationOn = false
        setOptionsSeen = false
        setOptionsAccepted = false
        setOptionsResult = nil
        getStateSeen = false
        getStateResult = nil
        actualFramesPresented = nil
        stateSampleAgeMs = nil
        uiTagHookInstalled = false
        hudlessTagActive = false
        uiAlphaTagActive = false
        uiColorAlphaTagActive = false
        uiDimensionsKnown = false
        uiDimensionsMatch = false
        uiRecompositionEnabled = false
        uiRecompositionForced = false
        gameUiRecompositionEnabled = false
        realFps = nil
        dlssFps = nil
        fpsSampleAgeMs = nil
        return
    end
    local wasLive = liveBridgeDetected
    nativeStatusDetected = true
    nativeStatusVersion = state.statusVersion
    liveBridgeDetected = state.bridgeReady
    synthesisFallbackActive = state.synthesisFallbackActive
    activeMode = state.mode
    activeMultiplier = state.multiplier
    activeDynamicTarget = state.dynamicTargetFrameRate
    activeDynamicExperimental56 = state.dynamicExperimental56
    activeGeneratedOnlyDebug = state.generatedOnlyDebug
    activePatchRoute = state.patchRoute
    appliedMode = state.appliedMode
    appliedMultiplier = state.appliedMultiplier
    appliedDynamicTarget = state.appliedDynamicTargetFrameRate
    requestPending = state.pending
    streamlineRebuildRequired = state.streamlineRebuildRequired
    gameFrameGenerationOn = state.gameFrameGenerationOn
    setOptionsSeen = state.setOptionsSeen
    setOptionsAccepted = state.setOptionsAccepted
    setOptionsResult = state.setOptionsResult
    getStateSeen = state.getStateSeen
    getStateResult = state.getStateResult
    actualFramesPresented = state.actualFramesPresented
    stateSampleAgeMs = state.stateSampleAgeMs
    uiTagHookInstalled = state.uiTagHookInstalled
    hudlessTagActive = state.hudlessTagActive
    uiAlphaTagActive = state.uiAlphaTagActive
    uiColorAlphaTagActive = state.uiColorAlphaTagActive
    uiDimensionsKnown = state.uiDimensionsKnown
    uiDimensionsMatch = state.uiDimensionsMatch
    uiRecompositionEnabled = state.uiRecompositionEnabled
    uiRecompositionForced = state.uiRecompositionForced
    gameUiRecompositionEnabled = state.gameUiRecompositionEnabled
    realFps = state.realFps
    dlssFps = state.dlssFps
    fpsSampleAgeMs = state.fpsSampleAgeMs
    if liveBridgeDetected and not wasLive then
        if activePatchRoute == "ota" then
            statusMessage = "Native bridge connected through the NVIDIA App OTA override. Multiplier changes apply live after the first clean Frame Generation enable."
        elseif activePatchRoute == "external" or activePatchRoute == "mixed" then
            statusMessage = "Native bridge connected through loaded external modules. Multiplier changes apply live after the first clean Frame Generation enable."
        else
            statusMessage = "Automatic native bridge connected. Multiplier changes apply live after the first clean Frame Generation enable."
        end
        print(MOD_NAME .. ": " .. statusMessage)
    elseif nativeStatusVersion < 7 then
        statusMessage = "Update RTX40MFG.asi; bridge protocol is outdated."
    elseif not liveBridgeDetected then
        statusMessage = "Waiting for the active DLSS-G wrapper and NGX module."
    end
end

local function chooseFixed(multiplier)
    selectedMode = "fixed"
    selectedMultiplier = multiplier
    saveConfig()
end

local function chooseDynamic()
    selectedMode = "dynamic"
    saveConfig()
end

registerForEvent("onInit", function()
    loadConfig()
    refreshBridgeStatus()
    if not nativeStatusDetected then
        statusMessage = "Waiting for the automatic native bridge."
    end
    print(MOD_NAME .. ": loaded; request is " .. describeRequest())
end)

registerForEvent("onOverlayOpen", function()
    overlayOpen = true
end)

registerForEvent("onOverlayClose", function()
    overlayOpen = false
end)

registerForEvent("onUpdate", function(deltaTime)
    bridgePollElapsed = bridgePollElapsed + deltaTime
    if bridgePollElapsed >= 0.5 then
        bridgePollElapsed = 0
        refreshBridgeStatus()
    end
end)

registerForEvent("onDraw", function()
    if not overlayOpen then
        return
    end

    ImGui.SetNextWindowSize(410, 320, ImGuiCond.FirstUseEver)
    ImGui.Begin(MOD_NAME)

    if liveBridgeDetected then
        local route = activePatchRoute == "ota" and "OTA"
            or (activePatchRoute == "both" and "Local + External"
            or (activePatchRoute == "external" and "External"
            or (activePatchRoute == "mixed" and "Mixed" or "Local")))
        ImGui.Text("Bridge: Connected (" .. route .. ")")
        local requested = activeMode == "dynamic"
            and ("Dynamic @ " .. (activeDynamicTarget == 0 and "refresh" or (tostring(activeDynamicTarget) .. " FPS"))
                .. (activeDynamicExperimental56 and " | max 6x*" or " | max 4x"))
            or (tostring(activeMultiplier) .. "x")
        ImGui.Text("Requested: " .. requested)

        local statusLabel = "Status"
        local statusValue = "Waiting"
        if not setOptionsSeen then
            statusValue = "Waiting for game"
        elseif not gameFrameGenerationOn then
            statusValue = "Off"
        elseif setOptionsResult == 21 then
            statusValue = "DLSS-G not initialized (21)"
        elseif setOptionsResult == 38 then
            statusValue = "Invalid state (38)"
        elseif setOptionsResult and not setOptionsAccepted then
            statusValue = "Error " .. tostring(setOptionsResult)
        elseif streamlineRebuildRequired then
            statusValue = "Re-enable Frame Generation"
        elseif requestPending then
            statusValue = "Waiting for clean enable"
        elseif getStateSeen and getStateResult == 0 and stateSampleAgeMs
            and stateSampleAgeMs <= 2500 and actualFramesPresented then
            statusLabel = "Actual"
            statusValue = actualFramesPresented > 0 and (tostring(actualFramesPresented) .. "x") or "Idle"
        elseif setOptionsAccepted then
            statusValue = "Applied"
        end
        if setOptionsResult == 39 then
            statusValue = statusValue .. " | VRAM low"
        end
        ImGui.Text(statusLabel .. ": " .. statusValue)
        if synthesisFallbackActive then
            ImGui.Text("3x-6x unavailable (DLL mismatch)")
        end
        if realFps and dlssFps and fpsSampleAgeMs and fpsSampleAgeMs <= 2000 then
            ImGui.Text("FPS: " .. tostring(math.floor(realFps + 0.5))
                .. " real | " .. tostring(math.floor(dlssFps + 0.5)) .. " DLSS")
        else
            ImGui.Text("FPS: Waiting")
        end
        local uiStatus = "Waiting for HUDless"
        if not uiTagHookInstalled then
            uiStatus = "Unavailable"
        elseif uiRecompositionEnabled and uiRecompositionForced then
            uiStatus = "Recomposition requested"
        elseif gameUiRecompositionEnabled and uiRecompositionEnabled then
            uiStatus = "Game managed"
        elseif hudlessTagActive and (uiAlphaTagActive or uiColorAlphaTagActive)
            and not uiDimensionsKnown then
            uiStatus = "Buffer size unknown"
        elseif hudlessTagActive and (uiAlphaTagActive or uiColorAlphaTagActive)
            and not uiDimensionsMatch then
            uiStatus = "Buffer size mismatch"
        elseif hudlessTagActive then
            uiStatus = "HUDless only"
        end
        ImGui.Text("UI: " .. uiStatus)
    elseif nativeStatusDetected then
        if nativeStatusVersion and nativeStatusVersion < 7 then
            ImGui.Text("Bridge: Update RTX40MFG.asi")
        else
            ImGui.Text("Bridge: Waiting for active DLSS-G modules")
        end
    else
        ImGui.Text("Bridge: Offline - check RTX40MFG.asi")
    end
    ImGui.Separator()

    ImGui.Text("Mode")
    if ImGui.RadioButton("Dynamic", selectedMode == "dynamic") then
        chooseDynamic()
    end
    ImGui.SameLine()
    if ImGui.RadioButton("2x", selectedMode == "fixed" and selectedMultiplier == 2) then
        chooseFixed(2)
    end
    ImGui.SameLine()
    if ImGui.RadioButton("3x", selectedMode == "fixed" and selectedMultiplier == 3) then
        chooseFixed(3)
    end
    ImGui.SameLine()
    if ImGui.RadioButton("4x", selectedMode == "fixed" and selectedMultiplier == 4) then
        chooseFixed(4)
    end
    ImGui.SameLine()
    if ImGui.RadioButton("5x*", selectedMode == "fixed" and selectedMultiplier == 5) then
        chooseFixed(5)
    end
    ImGui.SameLine()
    if ImGui.RadioButton("6x*", selectedMode == "fixed" and selectedMultiplier == 6) then
        chooseFixed(6)
    end

    if selectedMode == "fixed" and selectedMultiplier >= 5 then
        ImGui.Text("* Experimental")
    end

    if selectedMode == "dynamic" then
        local autoTarget = dynamicTargetFrameRate == 0
        local changed = false
        local experimental = dynamicExperimental56
        experimental, changed = ImGui.Checkbox("Allow Dynamic 5x / 6x*", experimental)
        if changed then
            dynamicExperimental56 = experimental
            saveConfig()
        end
        if dynamicExperimental56 then
            ImGui.Text("* Experimental")
        end
        autoTarget, changed = ImGui.Checkbox("Use display refresh", autoTarget)
        if changed then
            dynamicTargetFrameRate = autoTarget and 0 or lastCustomTarget
            saveConfig()
        end
        if dynamicTargetFrameRate > 0 then
            local target = dynamicTargetFrameRate
            target, changed = ImGui.SliderInt("Target FPS", target, 30, 360)
            if changed then
                dynamicTargetFrameRate = target
                lastCustomTarget = target
                saveConfig()
            end
        end
        ImGui.Text("VSync: Off")
    end

    local generatedOnly = generatedOnlyDebug
    local generatedOnlyChanged = false
    generatedOnly, generatedOnlyChanged = ImGui.Checkbox("Generated frames only (debug)", generatedOnly)
    if generatedOnlyChanged then
        generatedOnlyDebug = generatedOnly
        saveConfig()
    end

    ImGui.Separator()
    ImGui.Text("In-game Frame Generation: On")
    ImGui.TextWrapped("Multiplier changes apply live after the first clean Frame Generation enable. If Status says Re-enable Frame Generation, toggle it Off -> On or restart.")
    if string.find(statusMessage, "Could not", 1, true)
        or string.find(statusMessage, "Invalid", 1, true) then
        ImGui.TextWrapped(statusMessage)
    end

    ImGui.End()
end)
