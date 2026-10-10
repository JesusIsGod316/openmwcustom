local root = assert(arg and arg[1], 'package root argument required')
package.path = root .. '/?.lua;' .. package.path
local common = require('scripts.optimizedmw_route_recorder.common')
local checks = 0
local function check(condition, message)
    checks = checks + 1
    assert(condition, message)
end
local function rejects(fn, message)
    local ok = pcall(fn)
    check(not ok, message)
end
local function opts()
    return { schema = 1, session = string.rep('a', 32), route_name = 'existing', enabled = true,
        sample_hz = 20, max_seconds = 600, max_records = 60000, max_bytes = 16777216,
        require_ctrl_shift = false, smoke_duration_seconds = 20, quit_on_finish = true }
end

check(common.json({ z = 1, a = 'quote"\n\\' }) == '{"a":"quote\\"\\u000a\\\\","z":1}', 'JSON escaping/sorted fields')
check(common.json({ { id = 'one' }, { id = 'two' } }) == '[{"id":"one"},{"id":"two"}]', 'policy array encoding')
rejects(function() common.json({ [1] = 1, [3] = 3 }) end, 'sparse arrays rejected')
rejects(function() common.json({ [1] = 1, name = 'mixed' }) end, 'mixed table rejected')
rejects(function() common.json({ n = 0 / 0 }) end, 'NaN rejected')
rejects(function() common.json({ n = math.huge }) end, 'infinity rejected')
local cycle = {}; cycle.self = cycle
rejects(function() common.json(cycle) end, 'cycle rejected')
rejects(function() local o = opts(); o.session = '../../bad'; common.validateOptions(o) end, 'session identifier bounded')
rejects(function() local o = opts(); o.max_bytes = 16777217; common.validateOptions(o) end, 'byte ceiling enforced')

local lines = {}
local r = common.newRecorder(opts(), function(line) lines[#lines + 1] = line end)
check(not r:start({}, 0), 'cannot start before ready')
r:ready({}, 0); r:start({}, 1)
check(#lines == 2 and r.seq == 2, 'ready/start only immediate output')
r:push('sample', { position = { x = 1, y = 2, z = 3 } }, 2)
check(#lines == 2 and #r.buffered == 1, 'samples buffered during acquisition')
r:finish('user_finished', 3)
check(r.flags.complete and r.seq == 4, 'complete terminal footer')
check(not r:push('sample', {}, 3), 'no appends after closure')
check(not r:flush(1), 'bounded flush keeps pending footer')
check(r:flush(1) and #lines == 4, 'footer emitted last')
check(lines[4]:find('"expected_records":4', 1, true), 'footer record count matches all rows')
local tiny = opts(); tiny.max_records = 8
local bounded = common.newRecorder(tiny, function() end)
bounded:ready({}, 0); bounded:start({}, 1)
for i = 1, 5 do check(bounded:push('sample', {}, i + 1), 'within record budget') end
check(not bounded:push('sample', {}, 7), 'one record reserved for footer')
bounded:finish('user_finished', 8)
check(not bounded.flags.complete and bounded.seq == 8, 'overflow cannot qualify complete')

-- Mock the installed API shape. Setup movement/camera writes are permitted only
-- while waiting for a private save ACK; any write after START throws.
local now, sim, game, realDt = 10, 100, 1000, 0.05
local playerRecording = false
local writesDuringRecording = 0
local cdata = { movement = 0, sideMovement = 0, run = false, sneak = false, jump = false,
    use = 0, yawChange = 0, pitchChange = 0 }
local controls = setmetatable({}, {
    __index = cdata,
    __newindex = function(_, key, value)
        if playerRecording then writesDuringRecording = writesDuringRecording + 1; error('control write during recording') end
        cdata[key] = value
    end,
})
local cell = { id = 'default', name = 'Seyda Neen', isExterior = true, gridX = -2, gridY = -9 }
local storageData = {}
local keyBindingsData = {
    custom_F9 = { device = 'keyboard', button = 66, type = 'trigger', action = 'QuickLoad' },
    ordinary_W = { device = 'keyboard', button = 26, type = 'action', action = 'MoveForward' },
}
local keyBindings = {
    asTable = function() return keyBindingsData end,
    set = function(_, key, value) keyBindingsData[key] = value end,
}
local bridge = {
    setLifeTime = function() end,
    get = function(_, key) return storageData[key] end,
    getCopy = function(_, key) return storageData[key] end,
    set = function(_, key, value) storageData[key] = value end,
    asTable = function() return {} end,
}
local triggerCallbacks = {}
local resolved = { MoveForward = 0, MoveBackward = 0, MoveLeft = 0, MoveRight = 0 }
local pausedValue = false
local cameraMode, primaryCameraMode = 3, 1
local nativeSwitches = { vanitymode = true, playercontrols = true, playerlooking = true,
    playerviewswitch = true, playerjumping = true, playerfighting = true, playermagic = true }
local nativeSwitchWrites, cameraModeWrites = {}, {}
local refuseVanityWrite = false
local playerType = { CONTROL_SWITCH = { VanityMode = 'vanitymode' },
    getControlSwitch = function(_, key) return nativeSwitches[key] end,
    setControlSwitch = function(object, key, value)
        if playerRecording then writesDuringRecording = writesDuringRecording + 1; error('control switch write during recording') end
        nativeSwitchWrites[#nativeSwitchWrites + 1] = { object = object, key = key, value = value }
        if not refuseVanityWrite then nativeSwitches[key] = value end
    end }
local emitted, globalEvents = {}, {}
local displayedMessages = {}
local quitCount = 0
local vmeta = {}
local function vec(x, y, z) return setmetatable({ x = x, y = y, z = z }, vmeta) end
vmeta.__add = function(a, b) return vec(a.x + b.x, a.y + b.y, a.z + b.z) end
vmeta.__mul = function(a, b) return vec(a.x * b, a.y * b, a.z * b) end
local visualObject = { id = 'visual_door', recordId = 'door_visual', cell = cell,
    position = vec(1, 20, 3), rotation = { getAnglesZYX = function() return 0, 0, 0 end } }
local input = {
    KEY = { F6 = 63, F9 = 66, F10 = 67, F11 = 68 },
    triggers = { Activate = true, Jump = true, AutoMove = true, AlwaysRun = true, ToggleSneak = true,
        ToggleWeapon = true, ToggleSpell = true, TogglePOV = true, Inventory = true, Journal = true, GameMenu = true },
    getRangeActionValue = function(key) return resolved[key] end,
    getBooleanActionValue = function() return false end,
    getMouseMoveX = function() return 0 end, getMouseMoveY = function() return 0 end,
    registerTriggerHandler = function(key, callback) triggerCallbacks[key] = callback end,
}
local interfaces = {
    UI = { getMode = function() return nil end },
    Camera = { getPrimaryMode = function() return primaryCameraMode end },
    Controls = { overrideMovementControls = function(value)
        if playerRecording then writesDuringRecording = writesDuringRecording + 1; error('override during recording') end
        storageData.override = value
    end },
}
local api = {
    ['openmw.core'] = { getRealTime = function() return now end,
        getSimulationTime = function() return sim end, getGameTime = function() return game end,
        getRealFrameDuration = function() return realDt end, isWorldPaused = function() return pausedValue end,
        getGMST = function() return 192 end,
        sendGlobalEvent = function(name, data) globalEvents[#globalEvents + 1] = { name = name, data = data } end,
        quit = function() quitCount = quitCount + 1 end },
    ['openmw.input'] = input,
    ['openmw.types'] = { Player = playerType },
    ['openmw.self'] = { controls = controls, cell = cell, position = { x = 1, y = 2, z = 3 }, object = { id = 'player' },
        rotation = { getAnglesZYX = function() return 0, 0, 0 end } },
    ['openmw.camera'] = { getPosition = function() return vec(1, 2, 100) end,
        getYaw = function() return 0 end, getPitch = function() return 0 end,
        getExtraYaw = function() return 0 end, getExtraPitch = function() return 0 end,
        getRoll = function() return 0 end, getMode = function() return cameraMode end,
        MODE = { FirstPerson = 1, ThirdPerson = 2, Vanity = 3 },
        setMode = function(mode, force)
            if playerRecording then writesDuringRecording = writesDuringRecording + 1; error('camera mode write during recording') end
            cameraModeWrites[#cameraModeWrites + 1] = { mode = mode, force = force }
            cameraMode = mode
        end,
        getFieldOfView = function() return 1 end,
        getThirdPersonDistance = function() return 0 end,
        viewportToWorldVector = function() return vec(0, 1, 0) end,
        setYaw = function() check(not playerRecording, 'camera yaw only setup') end,
        setPitch = function() check(not playerRecording, 'camera pitch only setup') end },
    ['openmw.ui'] = { showMessage = function(text)
        displayedMessages[#displayedMessages + 1] = { text = text, wall = now }
    end },
    ['openmw.util'] = { vector2 = function(x, y) return { x = x, y = y } end },
    ['openmw.nearby'] = { castRenderingRay = function() return { hit = true, hitObject = visualObject, hitPos = vec(1, 20, 3) } end },
    ['openmw.async'] = { callback = function(_, fn) return fn end },
    ['openmw.interfaces'] = interfaces,
    ['openmw.storage'] = { LIFE_TIME = { GameSession = 1 }, playerSection = function(name)
        if name == 'OMWInputBindings' then return keyBindings end
        return bridge
    end },
}
local playerOptions = opts()
api['scripts.optimizedmw_route_recorder.options'] = playerOptions
for name, module in pairs(api) do package.loaded[name] = module end
local realPrint = print
print = function(line)
    emitted[#emitted + 1] = line
    if line:find('"event":"start"', 1, true) then playerRecording = true end
    if line:find('"event":"finish"', 1, true) then playerRecording = false end
end
local player = dofile(root .. '/scripts/optimizedmw_route_recorder/player.lua')
local function tick(seconds)
    realDt = seconds or 0.05
    now, sim, game = now + realDt, sim + realDt, game + realDt
    player.engineHandlers.onFrame(realDt)
end
player.eventHandlers.OptimizedMWRouteObserver({ ready = true })
player.eventHandlers.OptimizedMWRoutePolicy({ event = 'policy_ready', ready = true, qualified = true })
player.eventHandlers.OptimizedMWRoutePolicy({ event = 'actor_suppressed', reference_id = '1', record_id = 'rat' })
tick(); tick(2.1)
check(#emitted == 1 and emitted[1]:find('"event":"ready"', 1, true), 'ready after stable cell')
check(keyBindingsData.custom_F9 == nil and keyBindingsData.ordinary_W ~= nil, 'only private recorder-key Lua conflicts removed')
check(emitted[1]:find('"cleared_count":1', 1, true), 'cleared private Lua binding identity reported')
check(#nativeSwitchWrites == 1 and nativeSwitchWrites[1].key == 'vanitymode' and nativeSwitchWrites[1].value == false
    and nativeSwitchWrites[1].object == api['openmw.self'], 'setup disables only the self-player idle Vanity switch')
check(not nativeSwitches.vanitymode and nativeSwitches.playercontrols and nativeSwitches.playerlooking
    and nativeSwitches.playerviewswitch and nativeSwitches.playerjumping and nativeSwitches.playerfighting
    and nativeSwitches.playermagic, 'movement, looking, manual POV and gameplay control switches preserved')
check(cameraMode == primaryCameraMode and #cameraModeWrites == 1 and cameraModeWrites[1].force == true,
    'existing Vanity returns to selected first-person view during setup')
check(emitted[1]:find('"private_camera_setup"', 1, true) and emitted[1]:find('"vanity_disabled":true', 1, true)
    and emitted[1]:find('"verified":true', 1, true), 'READY reports verified private idle-camera preparation')
for _, modifiers in ipairs({ { withCtrl = true }, { withShift = true }, { withAlt = true }, { withSuper = true }, { withCtrl = true, withShift = true } }) do
    modifiers.code = 63
    player.engineHandlers.onKeyPress(modifiers); tick()
    check(not storageData.checkpoint_request, 'modified F6 cannot alter the route or movement mode')
end
player.engineHandlers.onKeyPress({ code = 63, withCtrl = false, withShift = false }); tick()
check(storageData.checkpoint_request and storageData.override, 'start first requests private save')
check(#emitted == 1, 'no START before save ACK')
local savingMessage = displayedMessages[#displayedMessages]
local messageCountBeforeAck = #displayedMessages
check(savingMessage.text:find('Saving the private route starting state', 1, true), 'saving setup state visibly announced')
storageData.checkpoint_ack = { request = storageData.checkpoint_request, ok = true,
    save = { directory = 'Gallus_Darkwind', filename = 'route.omwsave', description = 'checkpoint' } }
tick()
local recordedStartWall = now
check(#emitted == 2 and not storageData.override, 'ACK releases setup and starts')
check(emitted[2]:find('"checkpoint_save"', 1, true), 'START binds exact private checkpoint')
local recordingMessage = displayedMessages[#displayedMessages]
check(recordingMessage.wall - savingMessage.wall < 0.25, 'fixture ACK arrives inside former message rate limit')
check(#displayedMessages == messageCountBeforeAck + 1 and recordingMessage.text:find('Route recorder: Recording.', 1, true), 'fast save ACK immediately displays changed recording state')
resolved.MoveForward = 1; cdata.movement = 1
tick(); triggerCallbacks.Activate(); tick(0.8)
player.eventHandlers.OptimizedMWRouteActivation({ observed = true, source = 'engine_onActivate_actual_target',
    target = { id = 'actual_door', record_id = 'actual_record', position = { x = 1, y = 2, z = 3 } } })
player.eventHandlers.OptimizedMWRouteDoor({ id = 'door1', record_id = 'door', teleport = true })
player.engineHandlers.onKeyPress({ code = 66, withCtrl = true }); tick()
player.engineHandlers.onKeyPress({ code = 66 }); tick()
pausedValue = true; tick(); pausedValue = false; tick()
player.engineHandlers.onKeyPress({ code = 67, withShift = true }); tick()
local userFinishWall = now + 0.05
player.engineHandlers.onKeyPress({ code = 67 }); tick()
check(#emitted == 2, 'acquisition printed no sample rows')
player.eventHandlers.OptimizedMWRoutePolicy({ event = 'policy_stopped', ready = true, qualified = true })
for _ = 1, 5 do tick() end
check(quitCount == 1, 'quit only after footer flush')
check(writesDuringRecording == 0, 'movement/camera/control defaults preserved while recording')
check(#nativeSwitchWrites == 1 and #cameraModeWrites == 1, 'camera preparation performs no writes during acquisition')
local sampleCount, controlCount, doorCount, triggerCount, checkpointCount, seq = 0, 0, 0, 0, 0, 0
local priorTime = -1
for _, line in ipairs(emitted) do
    local n = tonumber(line:match('"seq":(%d+)'))
    check(n == seq + 1, 'strict sequence emission order')
    seq = n
    if line:find('"event":"sample"', 1, true) then sampleCount = sampleCount + 1 end
    if line:find('"event":"control"', 1, true) then controlCount = controlCount + 1 end
    if line:find('"event":"door"', 1, true) then doorCount = doorCount + 1 end
    if line:find('"event":"trigger"', 1, true) then triggerCount = triggerCount + 1 end
    if line:find('"event":"checkpoint"', 1, true) then checkpointCount = checkpointCount + 1 end
end
check(sampleCount <= 9, 'long frame creates no fabricated catch-up poses')
check(controlCount == 2, 'input changes kept independently from pose cadence')
check(doorCount == 1 and triggerCount == 2 and checkpointCount == 1, 'door/input/visual-intent/stage observations distinct')
local foundVisual, foundActual = false, false
for _, line in ipairs(emitted) do
    if line:find('"name":"ActivateTargetSnapshot"', 1, true) and line:find('"actual_request":false', 1, true) then foundVisual = true end
    if line:find('"event":"activation"', 1, true) and line:find('actual_door', 1, true) then foundActual = true end
end
check(foundVisual and foundActual, 'visual candidate and exact engine target remain separate')
check(emitted[#emitted]:find('"complete":true', 1, true), 'normal finished fixture complete')
check(emitted[#emitted]:find('"contains_pauses":true', 1, true), 'pause interval retained')
local endWall = tonumber(emitted[#emitted]:match('"acquisition_end_wall":([%d%.eE%+%-]+)'))
check(math.abs(endWall - (userFinishWall - recordedStartWall)) < 0.000001, 'modified F10 ignored; plain F10 sets the end boundary')
check(emitted[3]:find('"before_start":true', 1, true), 'initial hostile identity retained after START')
print = realPrint

-- Exact activation observation is a separate ENGINE callback and cannot consume
-- or replace a custom activation action. Invalid targets remain explicit.
local activationEvents = {}
package.loaded['openmw.types'] = {
    Player = playerType,
    Door = { objectIsInstance = function(object) return object.isDoor == true end,
        isTeleport = function() return true end, destCell = function() return cell end,
        destPosition = function() return vec(100, 200, 300) end,
        destRotation = function() return visualObject.rotation end },
    ESM4Door = { objectIsInstance = function() return false end },
}
local global = dofile(root .. '/scripts/optimizedmw_route_recorder/global.lua')
local actor = { type = playerType, sendEvent = function(_, name, data)
    activationEvents[#activationEvents + 1] = { name = name, data = data }
end }
global.engineHandlers.onPlayerAdded(actor); global.engineHandlers.onUpdate()
local actualObject = { id = 'custom_target', recordId = 'custom_activation', cell = cell,
    position = vec(7, 8, 9), rotation = visualObject.rotation, isValid = function() return true end }
check(global.engineHandlers.onActivate(actualObject, actor) == nil, 'actual target observer never consumes activation')
check(activationEvents[2].name == 'OptimizedMWRouteActivation' and activationEvents[2].data.target.id == 'custom_target', 'custom activation target captured exactly')
actualObject.isValid = function() return false end
global.engineHandlers.onActivate(actualObject, actor)
check(activationEvents[3].data.observed == false and activationEvents[3].data.error ~= nil, 'invalid actual target explicitly unresolved')
actualObject.isValid = function() return true end; actualObject.isDoor = true
package.loaded['openmw.types'].Door.destCell = function() error('destination not available') end
global.engineHandlers.onActivate(actualObject, actor)
check(activationEvents[#activationEvents].name == 'OptimizedMWRouteObserver' and activationEvents[#activationEvents].data.ready == false, 'missing door metadata invalidates observer coverage')

-- Menu API fixture proves new-slot saving, ACK, and failure qualification.
local saves = { ['original.omwsave'] = { description = 'Original' } }
local overwriteSlot
api['openmw.menu'] = {
    STATE = { Running = 1 }, getState = function() return 1 end,
    getCurrentSaveDir = function() return 'Gallus_Darkwind' end,
    getSaves = function() local out = {}; for k, v in pairs(saves) do out[k] = v end; return out end,
    saveGame = function(description, slot)
        overwriteSlot = slot
        saves['new.omwsave'] = { description = description, creationTime = 1234 }
    end,
}
package.loaded['openmw.menu'] = api['openmw.menu']
storageData.checkpoint_ack = nil
local menu = dofile(root .. '/scripts/optimizedmw_route_recorder/menu.lua')
menu.engineHandlers.onFrame()
check(storageData.checkpoint_ack.ok and storageData.checkpoint_ack.save.filename == 'new.omwsave', 'menu resolves unique newlycreated save')
check(overwriteSlot == nil and saves['original.omwsave'].description == 'Original', 'original copied slot never overwritten')
storageData.checkpoint_request = 'next'
api['openmw.menu'].saveGame = function() error('disk full') end
menu.engineHandlers.onFrame()
check(not storageData.checkpoint_ack.ok and storageData.checkpoint_ack.error:find('disk full', 1, true), 'save failure returns negative ACK')

-- Loading/reinitializing PLAYER discards its old in-memory samples. A retained
-- GameSession bridge prevents an apparently fresh, successful second recording.
local reentryLines = {}
print = function(line) reentryLines[#reentryLines + 1] = line end
local reentry = dofile(root .. '/scripts/optimizedmw_route_recorder/player.lua')
reentry.engineHandlers.onFrame(); reentry.engineHandlers.onFrame()
check(#reentryLines == 1 and reentryLines[1]:find('"event":"finish"', 1, true), 'context reentry emits terminal failure rather than new READY')
check(reentryLines[1]:find('"complete":false', 1, true) and reentryLines[1]:find('"lost_buffer":true', 1, true), 'reload cannot qualify lost in-memory recording')
check(quitCount == 2, 'context reentry exits naturally after failure export')
print = realPrint

-- Fresh private sessions: F11 aborts before READY and modified F11 is ignored.
storageData = {}
playerRecording = false
cdata.movement, cdata.sideMovement, cdata.jump = 0, 0, false
local earlyLines = {}
print = function(line) earlyLines[#earlyLines + 1] = line end
local early = dofile(root .. '/scripts/optimizedmw_route_recorder/player.lua')
early.engineHandlers.onKeyPress({ code = 68, withCtrl = true })
early.engineHandlers.onFrame()
check(#earlyLines == 0, 'modified F11 cannot abort waiting setup')
early.engineHandlers.onKeyPress({ code = 68 })
early.engineHandlers.onFrame()
check(#earlyLines == 1 and earlyLines[1]:find('"reason":"user_aborted"', 1, true), 'plain F11 aborts before READY')
check(earlyLines[1]:find('"complete":false', 1, true) ~= nil and quitCount == 3, 'early abort exports incomplete evidence and exits naturally')
print = realPrint

-- Legacy explicit modifier mode remains supported but is never the new default.
storageData = {}
playerOptions.require_ctrl_shift = true
local legacyLines = {}
print = function(line) legacyLines[#legacyLines + 1] = line end
local legacy = dofile(root .. '/scripts/optimizedmw_route_recorder/player.lua')
legacy.eventHandlers.OptimizedMWRouteObserver({ ready = true })
legacy.eventHandlers.OptimizedMWRoutePolicy({ event = 'policy_ready', ready = true, qualified = true })
legacy.engineHandlers.onFrame(); now, sim, game = now + 2.1, sim + 2.1, game + 2.1
legacy.engineHandlers.onFrame()
legacy.engineHandlers.onKeyPress({ code = 63 }); legacy.engineHandlers.onFrame()
check(storageData.checkpoint_request == nil, 'legacy explicit true refuses plain F6')
legacy.engineHandlers.onKeyPress({ code = 63, withCtrl = true, withShift = true }); legacy.engineHandlers.onFrame()
check(storageData.checkpoint_request ~= nil, 'legacy explicit true accepts Ctrl+Shift+F6')
legacy.engineHandlers.onKeyPress({ code = 68, withCtrl = true, withShift = true }); legacy.engineHandlers.onFrame()
check(storageData.override == false, 'aborting legacy setup releases temporary movement hold')
playerOptions.require_ctrl_shift = false
print = realPrint

-- Camera setup uses the same native APIs as the installed engine, gates READY,
-- and never changes an ordinary selected third-person view or unrelated switches.
local function cameraSetupFixture(mode, primary, refuse)
    storageData = {}
    playerRecording = false
    cameraMode, primaryCameraMode = mode, primary
    nativeSwitches.vanitymode = true
    refuseVanityWrite = refuse
    nativeSwitchWrites, cameraModeWrites = {}, {}
    local captured = {}
    print = function(line) captured[#captured + 1] = line end
    local fixture = dofile(root .. '/scripts/optimizedmw_route_recorder/player.lua')
    fixture.eventHandlers.OptimizedMWRouteObserver({ ready = true })
    fixture.eventHandlers.OptimizedMWRoutePolicy({ event = 'policy_ready', ready = true, qualified = true })
    fixture.engineHandlers.onFrame()
    now, sim, game = now + 2.1, sim + 2.1, game + 2.1
    fixture.engineHandlers.onFrame()
    return fixture, captured
end
local thirdPerson, thirdLines = cameraSetupFixture(2, 2, false)
check(cameraMode == 2 and #cameraModeWrites == 0, 'normal third-person view is never rewritten during setup')
check(thirdLines[1]:find('"event":"ready"', 1, true) ~= nil, 'ordinary third-person setup qualifies READY')
local orbit, orbitLines = cameraSetupFixture(3, 2, false)
check(cameraMode == 2 and #cameraModeWrites == 1 and cameraModeWrites[1].force == true,
    'existing Vanity preserves selected third-person primary mode')
check(orbitLines[1]:find('"restored_mode":2', 1, true) ~= nil, 'restored selected primary mode reported')
local failedCamera, failedLines = cameraSetupFixture(1, 1, true)
failedCamera.engineHandlers.onFrame()
check(#failedLines == 1 and failedLines[1]:find('"reason":"private_camera_setup_failed"', 1, true),
    'failed idle-camera readback exports failure instead of READY')
check(failedLines[1]:find('"complete":false', 1, true) and failedLines[1]:find('"verified":false', 1, true),
    'failed idle-camera disable can never qualify a route')
check(storageData.checkpoint_request == nil and cameraMode == 1 and #cameraModeWrites == 0,
    'failed readback neither requests a save nor changes ordinary camera mode')
local changedBeforeStart, changedLines = cameraSetupFixture(1, 1, false)
nativeSwitches.vanitymode, refuseVanityWrite = true, true
changedBeforeStart.engineHandlers.onKeyPress({ code = 63 })
changedBeforeStart.engineHandlers.onFrame(); changedBeforeStart.engineHandlers.onFrame()
check(#changedLines == 2 and changedLines[2]:find('"reason":"private_camera_setup_failed"', 1, true),
    'start rechecks idle-view switch before requesting private checkpoint')
check(storageData.checkpoint_request == nil and storageData.override ~= true,
    'failed start readback leaves movement free and creates no save request')
refuseVanityWrite = false
print = realPrint

-- Opt-in exterior native-input acquisition uses the actual PLAYER lifecycle.
-- The generic interaction/third-person fixtures above remain unchanged.
local recorderCollision = true
api['openmw.debug'] = { isCollisionEnabled = function() return recorderCollision end }
api['openmw.types'].Actor = { isOnGround = function() return true end, isSwimming = function() return false end,
    getCurrentSpeed = function() return 0 end }
package.loaded['openmw.debug'] = api['openmw.debug']
package.loaded['openmw.types'] = api['openmw.types']
local function compatibleFixture(exterior, mode, collision, cohort)
    storageData = {}; playerRecording = false
    cdata.movement, cdata.sideMovement, cdata.jump, cdata.use = 0, 0, false, 0
    cdata.run, cdata.sneak = false, false
    resolved.MoveForward = 0; pausedValue = false
    cell.isExterior = exterior ~= false
    cameraMode, primaryCameraMode = mode or 1, mode or 1
    recorderCollision = collision
    nativeSwitches.vanitymode, refuseVanityWrite = true, false
    local configuration = opts()
    configuration.playback_compatible, configuration.max_seconds = true, 180
    configuration.max_wall_seconds = 300
    configuration.movement_cohort = cohort or { model = 'native_input_displacement_with_visual_animations', private_override = true }
    package.loaded['scripts.optimizedmw_route_recorder.options'] = configuration
    local captured = {}
    print = function(line)
        captured[#captured + 1] = line
        if line:find('"event":"start"', 1, true) then playerRecording = true end
        if line:find('"event":"finish"', 1, true) then playerRecording = false end
    end
    local fixture = dofile(root .. '/scripts/optimizedmw_route_recorder/player.lua')
    local function policy(event, pending)
        fixture.eventHandlers.OptimizedMWRoutePolicy({session=configuration.session,event=event or 'policy_ready',ready=true,
            qualified=true,failed=false,failure_count=0,pending_disable=pending or 0,combat_before_start=0,combat_during_recording=0})
    end
    local function step(delta, simulationDelta)
        delta=delta or 0.05
        simulationDelta=simulationDelta or delta
        now,sim,game=now+delta,sim+simulationDelta,game+simulationDelta
        realDt=delta
        fixture.engineHandlers.onFrame(delta)
    end
    local function press(code) fixture.engineHandlers.onKeyPress({code=code}); step() end
    local function ack()
        storageData.checkpoint_ack={request=storageData.checkpoint_request,ok=true,
            save={directory='Gallus_Darkwind',filename='private-route.omwsave',description='new private checkpoint'}}
        step()
    end
    local function footer()
        for i=#captured,1,-1 do if captured[i]:find('"event":"finish"',1,true) then return captured[i] end end
    end
    fixture.eventHandlers.OptimizedMWRouteObserver({ready=true}); policy()
    step(); step(2.1)
    return {player=fixture,configuration=configuration,lines=captured,policy=policy,step=step,press=press,ack=ack,footer=footer}
end
local f=compatibleFixture(true,1,true)
local beforeReadyMarkerQuit=quitCount
f.press(66)
check(displayedMessages[#displayedMessages].text:find('Not recording. Press F6 to start first',1,true),
    'ready F9 explains that F6 must start recording before stage markers')
check(not playerRecording and #f.lines==1 and f.lines[1]:find('"event":"ready"',1,true) and not f.footer()
    and quitCount==beforeReadyMarkerQuit,
    'ready F9 neither starts nor finishes recording and never closes the game')
check(not storageData.checkpoint_request and not storageData.override,
    'ready F9 creates no private save request and leaves ordinary movement free')
f.press(63); f.ack()
f.press(66)
check(playerRecording and not f.footer() and quitCount==beforeReadyMarkerQuit
    and displayedMessages[#displayedMessages].text:find('Stage marker 1 recorded.',1,true),
    'F6 followed by F9 visibly marks the recording without stopping it')
f.press(67); f.policy('policy_stopped',0)
for _=1,5 do f.step() end
local readyRecoveryMarker=false
for _,line in ipairs(f.lines) do
    if line:find('"event":"checkpoint"',1,true) and line:find('"index":1',1,true) then readyRecoveryMarker=true end
end
check(readyRecoveryMarker and f.footer():find('"complete":true',1,true),
    'the first marker survives a completed acquisition after the ready-state reminder')

f=compatibleFixture(false,1,true)
f.press(63)
check(not storageData.checkpoint_request and not storageData.override and not playerRecording and #f.lines==1,
    'interior F6 rejection leaves compatible recorder ready and ordinary movement free')
check(displayedMessages[#displayedMessages].text:find('Go outside',1,true), 'interior rejection explains how to recover')
cell.isExterior=true; f.press(63); f.ack()
check(playerRecording and not storageData.override and #f.lines==2,
    'walking outside after rejected F6 can start the same private recording')
check(f.lines[1]:find('"movement_cohort"',1,true) and f.lines[2]:find('native_input_displacement_with_visual_animations',1,true)
    and f.lines[2]:find('"collision_enabled":true',1,true), 'READY and START retain native cohort and physical collision proof')
f.press(67); f.policy('policy_stopped',1)
for _=1,6 do f.step() end
check(not f.footer() and playerRecording, 'policy acknowledgement with pending suppression cannot close a successful capture')
f.policy('policy_stopped',0)
for _=1,5 do f.step() end
local footer=f.footer()
check(footer and footer:find('"complete":true',1,true) and footer:find('"movement_cohort"',1,true)
    and footer:find('"native_input_replay_compatible":true',1,true), 'fully drained explicit finish records the matching native movement cohort')

f=compatibleFixture(true,2,true); f.press(63)
check(not storageData.checkpoint_request and cameraMode==2 and not playerRecording,
    'third-person F6 rejection does not force the user camera or start saving')
cameraMode=1; f.press(63); f.ack()
check(playerRecording, 'selecting first person recovers from rejected start without relaunching')
f.press(68); f.step()

f=compatibleFixture(true,1,false); f.press(63)
check(not storageData.checkpoint_request and not playerRecording and recorderCollision==false,
    'collision-off start is refused without toggling collision')
recorderCollision=true; f.press(63); f.ack()
check(playerRecording, 'confirmed collision-enabled state allows subsequent start')
f.press(68); f.step()

f=compatibleFixture(true,1,nil); f.press(63)
check(not storageData.checkpoint_request and not playerRecording,
    'unavailable collision state cannot masquerade as enabled collision')
f.press(68); f.step()

f=compatibleFixture(true,1,true); f.press(63)
local originalRequest=storageData.checkpoint_request
cell.isExterior=false; f.ack()
check(not playerRecording and storageData.override==false and #f.lines==1,
    'save ACK rechecks exterior scope and releases setup after recoverable rejection')
cell.isExterior=true; f.press(63)
check(storageData.checkpoint_request~=originalRequest and storageData.checkpoint_request:find(':start:2',1,true),
    'retry uses a fresh request that the MENU save helper can process')
f.ack(); check(playerRecording, 'retry after changed save-ACK scope starts with a new checkpoint')
f.press(68); f.step()

f=compatibleFixture(true,1,true)
for attempt=1,8 do
    f.press(63)
    check(storageData.checkpoint_request==f.configuration.session..':start'..(attempt>1 and ':'..attempt or ''),
        'bounded save retry request '..attempt..' is compatible with the host admission')
    cell.isExterior=false; f.ack(); cell.isExterior=true
end
f.press(63); f.step()
check(f.footer():find('route_checkpoint_retry_budget_exceeded',1,true) and f.footer():find('"complete":false',1,true),
    'repeated post-save scope changes stop at eight private checkpoint saves')

f=compatibleFixture(true,1,true); f.policy('policy_ready',1); f.press(63)
check(not storageData.checkpoint_request and not playerRecording, 'F6 waits for confirmed suppression work to drain')
f.policy('policy_ready',0); f.press(63); f.ack()
check(playerRecording, 'drained start policy can recover in the same ready session')
f.press(67); f.policy('policy_stopped',1); f.step(5.1); f.step()
check(f.footer():find('"complete":false',1,true) and f.footer():find('policy_finish_ack_timeout',1,true),
    'pending final suppression eventually exports specific incomplete evidence')
check(f.footer():find('"observation_settle_timeout":true',1,true),
    'a requested final observer drain that really times out retains its explicit timeout flag')
f=compatibleFixture(true,1,true); f.press(63); f.ack(); f.press(67); f.press(68); f.step()
check(f.footer():find('user_aborted',1,true) and not f.footer():find('observation_settle_timeout',1,true),
    'F11 while an unacknowledged drain is still inside its deadline is an abort rather than a timeout')

f=compatibleFixture(true,1,true); f.press(63); f.ack(); cell.isExterior=false; f.step(); f.step()
check(f.footer():find('"complete":false',1,true) and f.footer():find('playback_scope_exterior_required',1,true),
    'interior transition during compatible acquisition cannot finish as a replayable route')
f=compatibleFixture(true,1,true); f.press(63); f.ack(); cameraMode=2; f.step(); f.step()
check(f.footer():find('"complete":false',1,true) and f.footer():find('playback_scope_first_person_required',1,true),
    'recorded camera mode change is explicitly incompatible')
f=compatibleFixture(true,1,true); f.press(63); f.ack(); recorderCollision=false; f.step(); f.step()
check(f.footer():find('"complete":false',1,true) and f.footer():find('playback_scope_collision_disabled',1,true),
    'collision toggle during acquisition is retained as incomplete evidence')
f=compatibleFixture(true,1,true); f.press(63); f.ack(); triggerCallbacks.Activate(); f.step()
check(f.footer():find('"complete":false',1,true) and f.footer():find('unsupported_input_trigger_activate',1,true),
    'actual resolved interaction trigger cannot silently enter the exterior motion preview')
check(not f.footer():find('observation_settle_timeout',1,true),
    'immediate unsupported interaction abort cannot imply a final observer drain was requested')

f=compatibleFixture(true,1,true); f.press(63); f.ack()
local beforeMarkerQuit=quitCount
f.press(66); f.step()
check(playerRecording and not f.footer() and quitCount==beforeMarkerQuit,
    'ordinary F9 retains the recording and does not stop or quit compatible acquisition')
triggerCallbacks.AutoMove(); cdata.movement=1; resolved.MoveForward=1; f.step()
check(playerRecording and not f.footer(), 'resolved AutoMove toggle is allowed without ending acquisition')
triggerCallbacks.AlwaysRun(); cdata.run=true; f.step()
check(playerRecording and not f.footer(), 'resolved AlwaysRun toggle is allowed without ending acquisition')
triggerCallbacks.ToggleSneak(); cdata.sneak=true; f.step()
check(playerRecording and not f.footer(), 'resolved ToggleSneak toggle is allowed without ending acquisition')
f.press(67)
-- These are native changes after the acquisition endpoint. They must not
-- overwrite F10 control proof or enter the acquired trigger/control timeline.
cdata.movement,cdata.run,cdata.sneak=0,false,false
triggerCallbacks.AutoMove(); f.policy('policy_stopped',0)
for _=1,5 do f.step() end
local function capturedRecord(event,pattern)
    for _,line in ipairs(f.lines) do
        if line:find('"event":"'..event..'"',1,true) and (not pattern or line:find(pattern,1,true)) then return line end
    end
end
check(capturedRecord('checkpoint','"index":1') and f.footer():find('"complete":true',1,true),
    'F9 marker and subsequent movement toggles survive a qualified explicit completed capture')
for _,name in ipairs({'AutoMove','AlwaysRun','ToggleSneak'}) do
    local line=capturedRecord('trigger','"name":"'..name..'"')
    check(line and line:find('"simulation":',1,true) and line:find('"playback_replayed_as":"resolved_native_control_snapshot"',1,true),
        name..' retains simulation-timed trigger evidence while playback consumes resolved controls')
end
local countAutoMove=0
for _,line in ipairs(f.lines) do
    if line:find('"event":"trigger"',1,true) and line:find('"name":"AutoMove"',1,true) then countAutoMove=countAutoMove+1 end
end
check(countAutoMove==1, 'native toggles after F10 do not extend the acquired trigger timeline into observer draining')
check(capturedRecord('control','"movement":1') and capturedRecord('control','"run":true')
    and capturedRecord('control','"sneak":true'), 'native movement/run/sneak changes following allowed toggles are all acquired')
check(f.lines[2]:find('"resolved_controls"',1,true) and f.lines[2]:find('"movement":0',1,true),
    'START exposes initial resolved native controls for supported toggle audit')
check(f.footer():find('"resolved_controls":{"jump":false,"movement":1',1,true)
    and f.footer():find('"run":true',1,true) and f.footer():find('"sneak":true',1,true),
    'FINISH exposes resolved controls frozen at F10 rather than changed post-drain native state')
check(not f.footer():find('observation_settle_timeout',1,true), 'fully acknowledged drain does not claim a settle timeout')
for _,name in ipairs({'Jump','ToggleWeapon','ToggleSpell','TogglePOV','Inventory','Journal','GameMenu'}) do
    f=compatibleFixture(true,1,true); f.press(63); f.ack(); triggerCallbacks[name](); f.step()
    check(f.footer():find('"complete":false',1,true) and f.footer():find('unsupported_input_trigger_'..name:lower(),1,true),
        'allowing resolved movement toggles preserves rejection of '..name)
    check(not f.footer():find('observation_settle_timeout',1,true),
        name..' immediate scope rejection is not mislabeled as an observer-drain timeout')
end
f=compatibleFixture(true,1,true); f.press(63); f.ack(); f.step(150.1)
check(displayedMessages[#displayedMessages].text:find('Press F10',1,true)
    and displayedMessages[#displayedMessages].text:find('cap is incomplete',1,true), 'duration warning asks for explicit finish before the bounded cap')
f.step(30); f.step()
check(f.footer():find('"complete":false',1,true) and f.footer():find('maximum_duration_reached',1,true),
    '180-second cap does not invent an explicit completed user recording')

f=compatibleFixture(true,1,true); f.press(63); f.ack()
local beforeWarning=#displayedMessages
f.step(170,5)
check(playerRecording and not f.footer() and #displayedMessages==beforeWarning,
    '170 wall seconds with only five simulation seconds neither warns nor reaches the compatible cap')
f.step(1,145.1)
check(displayedMessages[#displayedMessages].text:find('30 simulation seconds',1,true),
    'compatible warning follows 150 simulation seconds independently of the wall clock')
f.step(1,20)
check(displayedMessages[#displayedMessages].text:find('10 simulation seconds',1,true),
    'second compatible warning follows simulation elapsed rather than wall elapsed')
f.step(1,10); f.step()
check(f.footer():find('maximum_duration_reached',1,true) and f.footer():find('"complete":false',1,true),
    'compatible cap stops at 180 unpaused simulation seconds with independent wall timing')

f=compatibleFixture(true,1,true); f.press(63); f.ack(); f.step(200,5); f.press(67)
f.policy('policy_stopped',0); f.step(2,0.1); for _=1,4 do f.step() end
footer=f.footer()
local function numericField(line,name)
    return tonumber(line:match('"'..name..'":([%d%.eE+-]+)'))
end
check(footer and footer:find('"complete":true',1,true) and footer:find('"duration_clock":"unpaused_simulation"',1,true),
    'explicit simulation-bounded finish may exceed 180 wall seconds without altering its clock contract')
check(math.abs(numericField(footer,'duration_simulation')-5.05)<0.000001
    and math.abs(numericField(footer,'acquisition_end_simulation')-5.05)<0.000001,
    'compatible simulation duration ends at F10 and excludes actor-observer draining')
check(math.abs(numericField(footer,'duration_wall')-200.05)<0.000001
    and numericField(footer,'observation_settle_wall')>=2
    and numericField(footer,'observation_settle_simulation')>=0.2,
    'compatible footer reports acquisition and independent final observer-drain duration')
f=compatibleFixture(true,1,true); f.press(63); f.ack(); f.step(300.001,5); f.step()
check(f.footer():find('maximum_wall_duration_reached',1,true) and f.footer():find('"complete":false',1,true),
    'independent 300-second wall budget stops a compatible capture with only five simulation seconds')
f=compatibleFixture(true,1,true); f.press(63); f.ack()
f.player.engineHandlers.onKeyPress({code=67}); f.step(0.05,180.001); f.step()
check(f.footer():find('maximum_duration_reached',1,true) and f.footer():find('"complete":false',1,true),
    'queued F10 at the simulation cap cannot turn an over-budget route into a successful capture')
f=compatibleFixture(true,1,true); f.press(63); f.ack()
f.player.engineHandlers.onKeyPress({code=67}); f.step(300.001,5); f.step()
check(f.footer():find('maximum_wall_duration_reached',1,true) and f.footer():find('"complete":false',1,true),
    'queued F10 at the wall cap cannot bypass the independent wall budget')
f=compatibleFixture(true,1,true,{model='animation_displacement',private_override=false}); f.press(63); f.ack()
check(playerRecording and f.lines[2]:find('"native_input_replay_compatible":false',1,true),
    'explicit animation-movement acquisition is identified as a distinct control cohort')
f.press(67); f.policy('policy_stopped',0); for _=1,5 do f.step() end
check(f.footer():find('"model":"animation_displacement"',1,true) and f.footer():find('"private_override":false',1,true),
    'animation-movement control cohort survives through the final footer')
check(writesDuringRecording==0, 'compatible guards and metadata never write movement/camera/control defaults during acquisition')
print=realPrint
print('PASS route Lua recorder checks=' .. checks)
