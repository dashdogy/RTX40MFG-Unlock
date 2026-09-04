local MOD_NAME = "DLSS MFG"
local MOD_VERSION = "1.2.0.22"
local CONTROL_VERSION = 11
local STATUS_PROTOCOL = 18

-- CET intentionally sandboxes each mod's file I/O to its own directory. The
-- native core detects this dedicated frontend and uses the same local files;
-- no path escape or process-working-directory assumption is required.
local PATH_PAIRS = {{
    config = "RTX40MFG-Universal.json",
    status = "RTX40MFG-Universal.status.json"
}}

local activePaths = PATH_PAIRS[1]
local overlayOpen = false
local pollElapsed = 0
local lastDrawStatusPoll = -1
local state = nil
local controlsLoaded = false
local lastSaveOk = true
local statusMessage = ""
local unsupportedDynamicRetired = false

local control = {
    followGame = true,
    mode = "follow",
    multiplier = 2,
    dynamicTargetFrameRate = 0,
    dynamicExperimental56 = false,
    generatedOnlyDebug = false
}
local lastCustomTarget = 60

local function clamp(value, minimum, maximum)
    value = tonumber(value) or minimum
    if value < minimum then return minimum end
    if value > maximum then return maximum end
    return math.floor(value)
end

local function numberValue(source, name, fallback)
    if type(source) ~= "table" then return fallback end
    local value = tonumber(source[name])
    if value == nil then return fallback end
    return value
end

local function boolValue(source, name)
    return type(source) == "table" and source[name] == true
end

local function stringValue(source, name, fallback)
    if type(source) == "table" and type(source[name]) == "string"
        and source[name] ~= "" then
        return source[name]
    end
    return fallback
end

local function yesNo(value)
    return value and "yes" or "no"
end

local function readJson(path)
    local file = io.open(path, "r")
    if not file then return nil end
    local content = file:read("*all")
    file:close()
    if not content or content == "" then return nil end
    local ok, decoded = pcall(json.decode, content)
    if not ok or type(decoded) ~= "table" then return nil end
    return decoded
end

local function writeJson(path, value)
    local ok, encoded = pcall(json.encode, value)
    if not ok or type(encoded) ~= "string" then
        return false, "Could not encode the control request."
    end
    local file = io.open(path, "w")
    if not file then
        return false, "Could not write RTX40MFG-Universal.json."
    end
    file:write(encoded)
    file:write("\n")
    file:close()
    return true, ""
end

local function freshStatus(candidate)
    local decoded = readJson(candidate.status)
    if not decoded then return nil end
    local heartbeat = tonumber(decoded.heartbeat)
    if heartbeat and math.abs(os.time() - heartbeat) > 5 then
        return nil
    end
    return decoded
end

local function findStatus()
    for _, candidate in ipairs(PATH_PAIRS) do
        local decoded = freshStatus(candidate)
        if decoded then
            activePaths = candidate
            return decoded
        end
    end
    return nil
end

local function loadControlValues(source)
    if type(source) ~= "table" then return false end
    local multiplier = clamp(source.multiplier, 2, 6)
    local target = clamp(source.dynamicTargetFrameRate, 0, 1000)
    local mode = type(source.mode) == "string" and source.mode or "fixed"
    local followGame = source.followGame == true or mode == "follow"
    if not followGame and mode ~= "fixed" and mode ~= "dynamic" then
        return false
    end
    control.followGame = followGame
    control.mode = followGame and "follow" or mode
    control.multiplier = multiplier
    control.dynamicTargetFrameRate = target
    control.dynamicExperimental56 = source.dynamicExperimental56 == true
    control.generatedOnlyDebug = source.generatedOnlyDebug == true
    if target > 0 then lastCustomTarget = target end
    controlsLoaded = true
    return true
end

local function loadControl()
    for _, candidate in ipairs(PATH_PAIRS) do
        local decoded = readJson(candidate.config)
        if decoded and loadControlValues(decoded) then
            activePaths = candidate
            return true
        end
    end
    return false
end

local function safeMaximum()
    return clamp(numberValue(state, "safeMaximumMultiplier", 2), 2, 6)
end

local function dynamicCapabilityKnown()
    return boolValue(state, "dynamicMfgSupportKnown")
end

local function dynamicSupported()
    return dynamicCapabilityKnown()
        and boolValue(state, "dynamicMfgSupported")
end

local function describeControl()
    if control.followGame then return "Follow game" end
    if control.mode == "dynamic" then
        if control.dynamicTargetFrameRate == 0 then
            return "Dynamic at refresh rate"
        end
        return "Dynamic at " .. tostring(control.dynamicTargetFrameRate)
            .. " FPS"
    end
    return tostring(control.multiplier) .. "X"
end

local function saveControl()
    local maximum = safeMaximum()
    control.multiplier = clamp(control.multiplier, 2, maximum)
    control.dynamicTargetFrameRate = clamp(
        control.dynamicTargetFrameRate, 0, 1000)
    if control.followGame then
        control.mode = "follow"
        control.dynamicExperimental56 = false
    elseif control.mode ~= "dynamic" then
        control.mode = "fixed"
    end
    if maximum < 6 then control.dynamicExperimental56 = false end

    local saved, message = writeJson(activePaths.config, {
        followGame = control.followGame,
        mode = control.mode,
        multiplier = control.multiplier,
        dynamicTargetFrameRate = control.dynamicTargetFrameRate,
        dynamicExperimental56 = control.dynamicExperimental56,
        generatedOnlyDebug = control.generatedOnlyDebug,
        intervalLogging = true,
        version = CONTROL_VERSION
    })
    lastSaveOk = saved
    statusMessage = saved and (describeControl() .. " requested.") or message
    print(MOD_NAME .. ": " .. statusMessage)
    return saved
end

local function refreshStatus()
    state = findStatus()
    if not state then return end
    if not controlsLoaded then loadControlValues(state) end

    if control.mode == "dynamic" and dynamicCapabilityKnown()
        and not dynamicSupported() and not unsupportedDynamicRetired then
        control.followGame = true
        control.mode = "follow"
        control.dynamicExperimental56 = false
        unsupportedDynamicRetired = true
        saveControl()
    elseif dynamicSupported() then
        unsupportedDynamicRetired = false
    end
end

local function nvidiaMaximumText()
    if not state or not boolValue(state, "nvidiaCompatibilityResolved") then
        return "unavailable"
    end
    local tier = numberValue(state, "nvidiaCompatibilityTier", 0)
    if tier == 4 or tier == 6 then return tostring(tier) .. "X" end
    return "not listed"
end

local function availableMaximumText()
    if not state or not boolValue(state, "activeWrapperObserved") then
        return "detecting"
    end
    return tostring(safeMaximum()) .. "X"
end

local function versionText(prefix)
    return string.format("%d.%d.%d.%d",
        numberValue(state, prefix .. "VersionMajor", 0),
        numberValue(state, prefix .. "VersionMinor", 0),
        numberValue(state, prefix .. "VersionBuild", 0),
        numberValue(state, prefix .. "VersionPrivate", 0))
end

local function selectMode(action)
    if not action or action.enabled == false then return end
    if action.kind == "follow" then
        control.followGame = true
        control.mode = "follow"
        control.dynamicExperimental56 = false
    elseif action.kind == "fixed" then
        control.followGame = false
        control.mode = "fixed"
        control.multiplier = action.multiplier
    elseif action.kind == "dynamic" then
        control.followGame = false
        control.mode = "dynamic"
    end
    saveControl()
end

local function drawModeSelector()
    local labels = {"Follow game"}
    local actions = {{kind = "follow"}}
    local currentIndex = control.followGame and 0 or nil
    local maximum = safeMaximum()
    for multiplier = 2, maximum do
        table.insert(labels, tostring(multiplier) .. "X")
        table.insert(actions, {kind = "fixed", multiplier = multiplier})
        if not control.followGame and control.mode == "fixed"
            and control.multiplier == multiplier then
            currentIndex = #labels - 1
        end
    end

    local dynamicLabel = dynamicSupported() and "Dynamic"
        or (dynamicCapabilityKnown()
            and "Dynamic (unavailable)" or "Dynamic (checking...)")
    table.insert(labels, dynamicLabel)
    table.insert(actions, {
        kind = "dynamic",
        enabled = dynamicSupported()
    })
    if not control.followGame and control.mode == "dynamic" then
        currentIndex = #labels - 1
    end
    if currentIndex == nil then
        currentIndex = math.max(1, math.min(maximum - 1, #labels - 2))
    end

    local selected, changed = ImGui.Combo(
        "MFG mode", currentIndex, labels, #labels)
    if changed then selectMode(actions[selected + 1]) end

    if dynamicCapabilityKnown() and not dynamicSupported() then
        ImGui.Text("Dynamic mode is not supported by this game.")
    end
    if not control.followGame and control.mode == "dynamic"
        and dynamicSupported() then
        local locked = control.dynamicTargetFrameRate == 0
        local newLocked, lockChanged = ImGui.Checkbox(
            "Lock target to refresh rate", locked)
        if lockChanged then
            control.dynamicTargetFrameRate = newLocked and 0 or lastCustomTarget
            saveControl()
        end
        if control.dynamicTargetFrameRate > 0 then
            local target, targetChanged = ImGui.SliderInt(
                "Custom target FPS", control.dynamicTargetFrameRate, 1, 1000)
            if targetChanged then
                control.dynamicTargetFrameRate = target
                lastCustomTarget = target
                saveControl()
            end
        end
        if maximum >= 6 then
            local experimental, experimentalChanged = ImGui.Checkbox(
                "Enable Dynamic 5X/6X (experimental)",
                control.dynamicExperimental56)
            if experimentalChanged then
                control.dynamicExperimental56 = experimental
                saveControl()
            end
        end
    end
end

local function drawMainStatus()
    ImGui.Text("NVIDIA-listed maximum: " .. nvidiaMaximumText())
    local available = "Available maximum: " .. availableMaximumText()
    if state and boolValue(state, "compatibilityFallback")
        and boolValue(state, "activeWrapperObserved") then
        available = available .. " (safe fallback)"
    end
    ImGui.Text(available)
    ImGui.Text("Frame Generation: "
        .. (boolValue(state, "gameFrameGenerationOn") and "On" or "Off"))

    if boolValue(state, "gameFrameGenerationOn") then
        local realFps = numberValue(state, "realFpsMilli", 0)
        local dlssFps = numberValue(state, "dlssFpsMilli", 0)
        local age = numberValue(state, "fpsSampleAgeMs", 999999)
        if realFps > 0 and dlssFps > 0 and age <= 2000 then
            ImGui.Text(string.format("FPS: %.1f real | %.1f DLSS",
                realFps / 1000, dlssFps / 1000))
        else
            ImGui.Text("FPS: measuring...")
        end
    end

    if numberValue(state, "version", 0) < STATUS_PROTOCOL then
        ImGui.TextWrapped("Update RTX40MFG.asi and RTX40MFGCore.dll "
            .. "together; the loaded core uses an older protocol.")
    elseif not boolValue(state, "bridgeReady") then
        ImGui.Text("Waiting for an active DLSS-G pipeline.")
    end
    if boolValue(state, "streamlineRebuildRequired")
        or boolValue(state, "pipelineMayPredateDetour") then
        ImGui.TextWrapped("Toggle Frame Generation Off then On, or restart "
            .. "the game, to recreate the pipeline with this selection.")
    end
end

local function drawDebug()
    local generatedOnly, generatedChanged = ImGui.Checkbox(
        "Generated frames only", control.generatedOnlyDebug)
    if generatedChanged then
        control.generatedOnlyDebug = generatedOnly
        saveControl()
    end

    ImGui.Separator()
    ImGui.Text("Active route")
    ImGui.Text(string.format("Core protocol: %d | Bridge: %s",
        numberValue(state, "version", 0),
        boolValue(state, "bridgeReady") and "ready" or "not ready"))
    ImGui.Text(string.format("Fail-closed reason: %s (%d)",
        stringValue(state, "universalRouteFailureReason", "none"),
        numberValue(state, "universalRouteFailure", 0)))
    ImGui.TextWrapped("Wrapper: "
        .. stringValue(state, "activeWrapperPath", "waiting for a real call"))
    ImGui.Text(string.format("Wrapper version: %s | generation: %d",
        versionText("activeWrapper"),
        numberValue(state, "activeWrapperGeneration", 0)))
    ImGui.Text(string.format("Control: %s (%s) | State: %s (%s)",
        stringValue(state, "activeControlPath", "none"),
        stringValue(state, "activeControlDetour", "none"),
        stringValue(state, "activeStatePath", "none"),
        stringValue(state, "activeStateDetour", "none")))

    ImGui.Separator()
    ImGui.Text("Active provider")
    ImGui.TextWrapped("Provider: "
        .. stringValue(state, "activeProviderPath", "waiting for FG Create"))
    ImGui.Text(string.format("Provider version: %s | generation: %d",
        versionText("activeProvider"),
        numberValue(state, "activeProviderGeneration", 0)))
    ImGui.Text(string.format("Selected by: %s | Create: %s | Evaluate: %s",
        stringValue(state, "providerSelectionSource", "none"),
        stringValue(state, "providerCreateDetour", "none"),
        stringValue(state, "providerEvaluateDetour", "none")))
    ImGui.Text("Midpoint ready at first Create: "
        .. yesNo(boolValue(state, "midpointReadyAtFirstCreate")))

    ImGui.Separator()
    ImGui.Text("Control and lifecycle")
    ImGui.Text(string.format("Requested/applied revision: %d/%d",
        numberValue(state, "requestRevision", 0),
        numberValue(state, "appliedRevision", 0)))
    ImGui.Text(string.format("Route last call/accepted revision: %d/%d",
        numberValue(state, "activeLastCallRevision", 0),
        numberValue(state, "activeLastAcceptedRevision", 0)))
    ImGui.Text(string.format("FG: %s | Off accepted: %s | Release observed: %s",
        boolValue(state, "gameFrameGenerationOn") and "on" or "off",
        yesNo(boolValue(state, "frameGenerationOffAccepted")),
        yesNo(boolValue(state, "releaseObserved"))))
    ImGui.Text(string.format("Release entry: %s | Recreate required: %s",
        boolValue(state, "releaseEntryCurrent") and "covered" or "unavailable",
        yesNo(boolValue(state, "streamlineRebuildRequired"))))
    ImGui.Text("NVIDIA max: " .. nvidiaMaximumText()
        .. " | Available max: " .. availableMaximumText())
    if boolValue(state, "getStateSeen")
        and numberValue(state, "getStateResult", -1) == 0 then
        ImGui.Text(string.format("Runtime: %d presented | max %d generated",
            numberValue(state, "actualFramesPresented", 0),
            numberValue(state, "numFramesToGenerateMax", 0)))
    end

    ImGui.Separator()
    ImGui.Text("Temporal interval trace (always on)")
    ImGui.Text(string.format(
        "Log: %s | Samples: %d valid, %d invalid, %d dropped",
        boolValue(state, "intervalLogReady") and "ready" or "opening",
        numberValue(state, "intervalValidSamples", 0),
        numberValue(state, "intervalInvalidSamples", 0),
        numberValue(state, "intervalDroppedSamples", 0)))
    if numberValue(state, "intervalValidSamples", 0) > 0 then
        ImGui.Text(string.format(
            "Last interval: count %d | index %d | position %d/%d",
            numberValue(state, "intervalLastCount", 0),
            numberValue(state, "intervalLastIndex", 0),
            numberValue(state, "intervalLastPositionNumerator", 0),
            numberValue(state, "intervalLastPositionDenominator", 0)))
    end
    local trace = stringValue(state, "intervalLogFile", "")
    if trace ~= "" then ImGui.Text("Trace: %TEMP%\\" .. trace) end
end

registerForEvent("onInit", function()
    loadControl()
    refreshStatus()
    statusMessage = "Loaded V" .. MOD_VERSION .. "."
    print(MOD_NAME .. ": " .. statusMessage)
end)

registerForEvent("onOverlayOpen", function()
    overlayOpen = true
end)

registerForEvent("onOverlayClose", function()
    overlayOpen = false
end)

registerForEvent("onUpdate", function(deltaTime)
    pollElapsed = pollElapsed + (tonumber(deltaTime) or 0)
    if pollElapsed >= 0.5 then
        pollElapsed = 0
        refreshStatus()
    end
end)

registerForEvent("onDraw", function()
    if not overlayOpen then return end

    -- onDraw can run while CET is still waiting to fire onInit/onUpdate. Poll
    -- here as well so opening the overlay early never produces a false backend
    -- failure while native telemetry is already available.
    if not state then
        local now = os.clock()
        if now - lastDrawStatusPoll >= 0.5 then
            lastDrawStatusPoll = now
            refreshStatus()
        end
    end

    ImGui.SetNextWindowSize(660, 640, ImGuiCond.FirstUseEver)
    ImGui.Begin(MOD_NAME)
    if not state then
        ImGui.Text("Waiting for RTX40MFG telemetry...")
        ImGui.TextWrapped("If this remains after the main menu has loaded, "
            .. "fully restart the game and verify the native files.")
        ImGui.End()
        return
    end

    drawMainStatus()
    ImGui.Separator()
    drawModeSelector()
    if not lastSaveOk then ImGui.TextWrapped(statusMessage) end

    ImGui.Separator()
    if ImGui.CollapsingHeader("Debug") then drawDebug() end
    ImGui.End()
end)
