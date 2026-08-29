local MOD_NAME = "RTX 40 MFG Unlock"
local CONFIG_PATH = "config.json"
local STATUS_PATH = "bridge_status.json"
local VALID_MULTIPLIERS = { [2] = true, [3] = true, [4] = true }

local overlayOpen = false
local selectedMode = "fixed"
local selectedMultiplier = 2
local dynamicTargetFrameRate = 0
local lastCustomTarget = 120
local nativeStatusDetected = false
local nativeStatusVersion = nil
local liveBridgeDetected = false
local activeMode = nil
local activeMultiplier = nil
local activeDynamicTarget = nil
local activePatchRoute = nil
local appliedMode = nil
local appliedMultiplier = nil
local appliedDynamicTarget = nil
local requestPending = false
local gameFrameGenerationOn = false
local setOptionsSeen = false
local setOptionsAccepted = false
local setOptionsResult = nil
local getStateSeen = false
local getStateResult = nil
local actualFramesPresented = nil
local stateSampleAgeMs = nil
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
            bridgeReady = statusVersion >= 4 and data.bridgeReady == true,
            multiplier = multiplier,
            mode = mode,
            dynamicTargetFrameRate = target or 0,
            patchRoute = route,
            appliedMode = data.appliedMode or mode,
            appliedMultiplier = tonumber(data.appliedMultiplier) or multiplier,
            appliedDynamicTargetFrameRate = tonumber(data.appliedDynamicTargetFrameRate) or (target or 0),
            pending = data.pending == true,
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
        version = 5
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
        activeMode = nil
        activeMultiplier = nil
        activeDynamicTarget = nil
        activePatchRoute = nil
        appliedMode = nil
        appliedMultiplier = nil
        appliedDynamicTarget = nil
        requestPending = false
        gameFrameGenerationOn = false
        setOptionsSeen = false
        setOptionsAccepted = false
        setOptionsResult = nil
        getStateSeen = false
        getStateResult = nil
        actualFramesPresented = nil
        stateSampleAgeMs = nil
        return
    end
    local wasLive = liveBridgeDetected
    nativeStatusDetected = true
    nativeStatusVersion = state.statusVersion
    liveBridgeDetected = state.bridgeReady
    activeMode = state.mode
    activeMultiplier = state.multiplier
    activeDynamicTarget = state.dynamicTargetFrameRate
    activePatchRoute = state.patchRoute
    appliedMode = state.appliedMode
    appliedMultiplier = state.appliedMultiplier
    appliedDynamicTarget = state.appliedDynamicTargetFrameRate
    requestPending = state.pending
    gameFrameGenerationOn = state.gameFrameGenerationOn
    setOptionsSeen = state.setOptionsSeen
    setOptionsAccepted = state.setOptionsAccepted
    setOptionsResult = state.setOptionsResult
    getStateSeen = state.getStateSeen
    getStateResult = state.getStateResult
    actualFramesPresented = state.actualFramesPresented
    stateSampleAgeMs = state.stateSampleAgeMs
    if liveBridgeDetected and not wasLive then
        if activePatchRoute == "ota" then
            statusMessage = "Native bridge connected through the NVIDIA App OTA override. Changes apply without restarting."
        elseif activePatchRoute == "external" or activePatchRoute == "mixed" then
            statusMessage = "Native bridge connected through loaded external modules. Changes apply without restarting."
        else
            statusMessage = "Automatic native bridge connected. Changes apply without restarting."
        end
        print(MOD_NAME .. ": " .. statusMessage)
    elseif nativeStatusVersion < 4 then
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

    ImGui.SetNextWindowSize(410, 245, ImGuiCond.FirstUseEver)
    ImGui.Begin(MOD_NAME)

    if liveBridgeDetected then
        local route = activePatchRoute == "ota" and "OTA"
            or (activePatchRoute == "both" and "Local + External"
            or (activePatchRoute == "external" and "External"
            or (activePatchRoute == "mixed" and "Mixed" or "Local")))
        ImGui.Text("Bridge: Connected (" .. route .. ")")
        local requested = activeMode == "dynamic"
            and ("Dynamic @ " .. (activeDynamicTarget == 0 and "refresh" or (tostring(activeDynamicTarget) .. " FPS")))
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
        elseif requestPending then
            statusValue = "Applying..."
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
    elseif nativeStatusDetected then
        if nativeStatusVersion and nativeStatusVersion < 4 then
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

    if selectedMode == "dynamic" then
        local autoTarget = dynamicTargetFrameRate == 0
        local changed = false
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

    ImGui.Separator()
    ImGui.Text("In-game Frame Generation: On")
    if string.find(statusMessage, "Could not", 1, true)
        or string.find(statusMessage, "Invalid", 1, true) then
        ImGui.TextWrapped(statusMessage)
    end

    ImGui.End()
end)
