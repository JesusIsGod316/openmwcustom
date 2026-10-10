-- User-driven acquisition: setup briefly holds movement/camera for a private MENU
-- checkpoint and disables idle Vanity in this private session. Recording itself
-- does not write movement, camera, or control switches.
local core = require('openmw.core')
local input = require('openmw.input')
local camera = require('openmw.camera')
local self = require('openmw.self')
local ui = require('openmw.ui')
local async = require('openmw.async')
local nearby = require('openmw.nearby')
local util = require('openmw.util')
local storage = require('openmw.storage')
local types = require('openmw.types')
local I = require('openmw.interfaces')
local common = require('scripts.optimizedmw_route_recorder.common')
local options = common.validateOptions(require('scripts.optimizedmw_route_recorder.options'))
assert(options.playback_compatible == nil or type(options.playback_compatible) == 'boolean', 'Invalid playback-compatible recording option')
local playbackCompatible = options.playback_compatible == true
local maxWallSeconds = playbackCompatible and (options.max_wall_seconds or 300) or nil
if playbackCompatible then
    assert(options.max_seconds <= 180, 'Playback-compatible recording exceeds the 180-second budget')
    assert(common.finite(maxWallSeconds) and maxWallSeconds > 0 and maxWallSeconds <= 300,
        'Playback-compatible recording exceeds the 300-second wall budget')
    local cohort = options.movement_cohort
    assert(type(cohort) == 'table' and type(cohort.private_override) == 'boolean'
        and ((cohort.model == 'native_input_displacement_with_visual_animations' and cohort.private_override == true)
            or (cohort.model == 'animation_displacement' and cohort.private_override == false)), 'Invalid private movement cohort')
end
local debugAvailable, debugApi = pcall(require, 'openmw.debug')
if not debugAvailable then debugApi = nil end
local recorder = common.newRecorder(options, print)
local wallOrigin, startWall, startSim, startGame
local stableSince, stableCell
local frame = 0
local lastSample = -math.huge
local lastControls, lastCell, lastPaused
local lastControlSequence
local checkpoint = 0
local pendingCommand
local observer = { ready = false }
local policy = { ready = false }
local policyCounts = { suppressed_before_start = 0, combat_before_start = 0 }
local suppressedLogged = {}
local suppressedBeforeStart = {}
local registeredTriggers = false
local lastMessage = -math.huge
local lastMessageText
local bridge = storage.playerSection('OptimizedMWRouteRecorder:' .. options.session)
bridge:setLifeTime(storage.LIFE_TIME.GameSession)
local reenteredSession = bridge:get('ready_written') == true
local checkpointRequest, checkpointWaitWall, checkpointSave, checkpointPose
local checkpointAttempts = 0
local setupFrozen = false
local closing
local privateBindings = { cleared_count = 0, cleared = {} }
local privateCamera = { source = 'private_setup_vanity_control_switch',
    vanity_disabled = false, verified = false, restored_first_person = false }
local keySetupDone = false
local activationCounts = { input_attempts = 0, visual_known = 0, visual_unknown = 0, actual_known = 0, actual_unknown = 0 }
local durationWarnings = {}
local function preparePrivateLuaBindings()
    if keySetupDone or not options.enabled then return end
    local keys = { options.key_start or 'F6', options.key_checkpoint or 'F9',
        options.key_finish or 'F10', options.key_abort or 'F11' }
    local reserved = {}
    for _, name in ipairs(keys) do
        local code = input.KEY[name]
        if not code then error('Unknown recorder key: ' .. tostring(name)) end
        if reserved[code] then error('Recorder keys must be distinct') end
        reserved[code] = true
    end
    local bindings = storage.playerSection('OMWInputBindings')
    local changes = {}
    for id, binding in pairs(bindings:asTable()) do
        if type(binding) == 'table' and binding.device == 'keyboard' and reserved[binding.button] then
            if type(id) ~= 'string' or #id > 128 then error('Unsafe private keyboard binding identifier') end
            changes[#changes + 1] = { id = id, scancode = binding.button }
            if #changes > 8 then error('More than eight conflicting private Lua keyboard bindings; setup refused') end
        end
    end
    table.sort(changes, function(a, b) return a.id < b.id end)
    for _, change in ipairs(changes) do bindings:set(change.id, nil) end
    privateBindings.cleared_count, privateBindings.cleared = #changes, changes
    keySetupDone = true
end

local function preparePrivateCamera()
    -- Only setup may change the idle-view switch or leave an existing Vanity orbit.
    -- Looking, manual POV, and all movement/action switches remain untouched.
    privateCamera.verified = false
    local ok, failure = pcall(function()
        if recorder.state ~= 'waiting' and recorder.state ~= 'ready' then
            error('Private camera preparation outside setup')
        end
        local Player = types.Player
        local key = Player.CONTROL_SWITCH.VanityMode
        local before = Player.getControlSwitch(self, key)
        if type(before) ~= 'boolean' then error('Vanity control switch has no boolean readback') end
        if privateCamera.previous_vanity_enabled == nil then privateCamera.previous_vanity_enabled = before end
        if before then Player.setControlSwitch(self, key, false) end
        privateCamera.vanity_disabled = Player.getControlSwitch(self, key) == false
        if not privateCamera.vanity_disabled then error('Idle Vanity disable failed readback') end
        local mode = camera.getMode()
        if not common.finite(mode) then error('Private camera mode is invalid') end
        if privateCamera.initial_camera_mode == nil then privateCamera.initial_camera_mode = mode end
        if mode == camera.MODE.Vanity then
            local primary = I.Camera and I.Camera.getPrimaryMode and I.Camera.getPrimaryMode()
            if primary ~= camera.MODE.FirstPerson and primary ~= camera.MODE.ThirdPerson then
                error('Selected primary view unavailable while leaving Vanity')
            end
            camera.setMode(primary, true)
            if camera.getMode() ~= primary then error('Leaving Vanity failed readback') end
            privateCamera.restored_mode = primary
            privateCamera.restored_first_person = primary == camera.MODE.FirstPerson
        end
        privateCamera.verified = true
    end)
    if not ok then privateCamera.error = tostring(failure):sub(1, 512) end
    return ok
end

local function message(text)
    -- Setup/marker messages are intentionally outside performance acceptance.
    local now = core.getRealTime()
    if text ~= lastMessageText or now - lastMessage > 0.25 then
        ui.showMessage('Route recorder: ' .. text)
        lastMessage, lastMessageText = now, text
    end
end
local function keyLabel(name, fallback)
    return (options.require_ctrl_shift == true and 'Ctrl+Shift+' or '') .. (options[name] or fallback)
end
local function relativeWall(now) return math.max(0, now - (wallOrigin or now)) end
local function cellSnapshot(cell)
    if not cell then return nil end
    return { id = cell.id, name = cell.name, exterior = cell.isExterior,
        grid_x = cell.isExterior and cell.gridX or nil, grid_y = cell.isExterior and cell.gridY or nil }
end
local function vector(v) return { x = v.x, y = v.y, z = v.z } end
local function paused() return core.isWorldPaused() or (I.UI and I.UI.getMode() ~= nil) end
local function readPhysical(fn, object)
    if type(fn) ~= 'function' then return nil end
    local ok, value
    if object == nil then ok, value = pcall(fn) else ok, value = pcall(fn, object) end
    if ok and (type(value) == 'boolean' or common.finite(value)) then return value end
    return nil
end
local function physicalState()
    local actor, collision = types.Actor or {}, nil
    if debugApi then collision = readPhysical(debugApi.isCollisionEnabled) end
    return { collision_enabled = collision, on_ground = readPhysical(actor.isOnGround, self),
        swimming = readPhysical(actor.isSwimming, self), current_speed = readPhysical(actor.getCurrentSpeed, self) }
end
local function captureScope()
    return { playback_compatible = playbackCompatible, exterior_first_person_only = playbackCompatible,
        max_seconds = options.max_seconds, max_wall_seconds = maxWallSeconds, explicit_finish_required = true,
        duration_clock = playbackCompatible and 'unpaused_simulation' or 'wall',
        native_input_replay_compatible = playbackCompatible and options.movement_cohort.model == 'native_input_displacement_with_visual_animations'
            and options.movement_cohort.private_override == true,
        supported_control_triggers = playbackCompatible and { 'AutoMove', 'AlwaysRun', 'ToggleSneak' } or nil,
        unsupported_actions = playbackCompatible and 'activation, doors, jumps, attacks, menus, view changes, weapon/spell toggles' or nil }
end
local function scopeFailure()
    if not playbackCompatible then return nil end
    if not self.cell or not self.cell.isExterior then return 'exterior_required', 'Go outside before starting; this route must remain outdoors.' end
    if camera.getMode() ~= camera.MODE.FirstPerson then return 'first_person_required', 'Select first-person view before starting.' end
    for _, value in ipairs({ camera.getExtraYaw(), camera.getExtraPitch(), camera.getRoll() }) do
        if not common.finite(value) or math.abs(value) > 0.000001 then
            return 'extra_camera_layer', 'Return to ordinary first-person view before starting.'
        end
    end
    local collision = physicalState().collision_enabled
    if collision ~= true then
        return collision == false and 'collision_disabled' or 'collision_state_unavailable',
            'Ordinary player collision must be confirmed enabled before recording.'
    end
    return nil
end
local function startGuard()
    local failure, explanation = scopeFailure()
    if failure then message(explanation); return false end
    if playbackCompatible then
        if policy.session ~= options.session or not observer.ready or not policy.ready or not policy.qualified or policy.failed
            or policy.pending_disable ~= 0 or policy.failure_count ~= 0 then
            message('Wait for confirmed hostile suppression and observer readiness, then press ' .. keyLabel('key_start', 'F6') .. ' again.')
            return false
        end
        if policy.combat_before_start ~= 0 or policy.combat_during_recording ~= 0 or policyCounts.combat_before_start ~= 0 then
            message('Combat was observed during setup. Restart this private recording from an outdoor save.')
            return false
        end
    end
    return true
end
local function releaseSetup()
    if setupFrozen and I.Controls then I.Controls.overrideMovementControls(false) end
    setupFrozen = false
end
local function holdSetup()
    if not setupFrozen then return end
    self.controls.movement, self.controls.sideMovement, self.controls.jump = 0, 0, false
    camera.setYaw(checkpointPose.camera.yaw)
    camera.setPitch(checkpointPose.camera.pitch)
end
local function pose()
    local z, y, x = self.rotation:getAnglesZYX()
    local w = core.weather and core.weather.getCurrent(self.cell)
    return {
        position = vector(self.position), rotation = { z = z, y = y, x = x }, cell = cellSnapshot(self.cell),
        camera = { position = vector(camera.getPosition()), yaw = camera.getYaw(), pitch = camera.getPitch(),
            extra_yaw = camera.getExtraYaw(), extra_pitch = camera.getExtraPitch(), roll = camera.getRoll(),
            mode = camera.getMode(), fov = camera.getFieldOfView() },
        weather = w and w.id or nil,
    }
end
local function controlSnapshot()
    local c = self.controls
    return {
        controls = { movement = c.movement, side_movement = c.sideMovement,
            run = c.run, sneak = c.sneak, jump = c.jump, use = c.use,
            yaw_change = c.yawChange, pitch_change = c.pitchChange },
        actions = {
            forward = input.getRangeActionValue('MoveForward'), backward = input.getRangeActionValue('MoveBackward'),
            left = input.getRangeActionValue('MoveLeft'), right = input.getRangeActionValue('MoveRight'),
            run = input.getBooleanActionValue('Run'), sneak = input.getBooleanActionValue('Sneak'),
            use = input.getBooleanActionValue('Use'),
        },
        mouse = { x = input.getMouseMoveX(), y = input.getMouseMoveY() },
    }
end
local function clocks(now)
    return { f = frame, wall_since_start = now - startWall,
        simulation = core.getSimulationTime() - startSim, game = core.getGameTime() - startGame,
        dt_real = core.getRealFrameDuration() }
end
local function policySummary()
    local summary = {}
    for key, value in pairs(policy) do if key ~= 'suppressed_actors' then summary[key] = value end end
    summary.suppressed_manifest_records = #(policy.suppressed_actors or {})
    return summary
end
local function finalize(reason, now)
    if recorder.state == 'exporting' or recorder.state == 'finished' then return end
    recorder.flags.door_observer_ready = observer.ready == true
    if not observer.ready then recorder.flags.activation_target_unresolved = true end
    recorder.flags.policy_ready = policy.ready == true
    recorder.flags.policy_qualified = policy.qualified == true
    if policy.failed or policy.qualified ~= true or (policy.pending_disable or 0) ~= 0
        or (policy.failure_count or 0) ~= 0 then recorder.flags.combat_invalid = true end
    if playbackCompatible and closing and (not closing.acknowledged or policy.pending_disable ~= 0) then
        if reason == 'user_finished' or reason == 'smoke_finished' then reason = 'policy_finish_not_drained' end
    end
    if playbackCompatible and (reason == 'user_finished' or reason == 'smoke_finished') then
        local failure = scopeFailure()
        if failure then recorder.flags.playback_scope_invalid = true; reason = 'playback_scope_' .. failure end
    end
    releaseSetup()
    -- Compatible acquisition ends at F10, before its independent actor-observer
    -- drain. The final record clock still includes the drain and export time.
    local endWall = playbackCompatible and closing and closing.end_wall or now
    local endSim = playbackCompatible and closing and closing.end_sim or core.getSimulationTime()
    local endGame = playbackCompatible and closing and closing.end_game or core.getGameTime()
    recorder:finish(reason, relativeWall(now), {
        duration_wall = startWall and endWall - startWall or 0,
        duration_simulation = startSim and endSim - startSim or 0,
        duration_game = startGame and endGame - startGame or 0,
        frames = frame, route_name = options.route_name, observer = observer, policy = policySummary(),
        private_lua_key_bindings = privateBindings,
        private_camera_setup = privateCamera,
        movement_cohort = options.movement_cohort, capture_scope = captureScope(), physical_state = physicalState(),
        resolved_controls = closing and closing.end_controls or controlSnapshot().controls,
        activation_coverage = activationCounts,
        acquisition_end_wall = closing and closing.end_wall - startWall or (startWall and now - startWall or 0),
        acquisition_end_simulation = closing and closing.end_sim - startSim or (startSim and endSim - startSim or 0),
        observation_settle_wall = closing and now - closing.end_wall or 0,
        observation_settle_simulation = closing and core.getSimulationTime() - closing.end_sim or 0,
        observation_settle_frames = closing and closing.frames or 0,
    })
    core.sendGlobalEvent('OptimizedMWRouteRecording', { active = false, session = options.session, t_wall = closing and closing.end_wall or now })
    if reason == 'user_aborted' then message('Aborted. Exporting incomplete evidence; the game will close.')
    else message('Recording closed; exporting. Please leave the game open until it exits.') end
end
local function finish(reason, now)
    if (reason == 'user_finished' or reason == 'smoke_finished') and recorder.state == 'recording' then
        if closing then return end
        if playbackCompatible then
            if core.getSimulationTime() - startSim >= options.max_seconds then
                finalize('maximum_duration_reached', now)
                return
            elseif now - startWall >= maxWallSeconds then
                finalize('maximum_wall_duration_reached', now)
                return
            end
        end
        closing = { reason = reason, end_wall = now, end_sim = core.getSimulationTime(),
            end_game = core.getGameTime(), end_controls = controlSnapshot().controls, frames = 0, acknowledged = false }
        core.sendGlobalEvent('OptimizedMWRouteRecording', { active = false, session = options.session, t_wall = now })
        message('Finishing; waiting for the final actor observations.')
        return
    end
    finalize(reason, now)
end
local function push(event, payload, now)
    if not recorder:push(event, payload, relativeWall(now)) then finish('capture_budget_exceeded', now) end
end
local function beginRecording(now)
    if recorder.state ~= 'ready' then return end
    if paused() or not self.cell then message('Close menus and wait for the starting cell before starting.'); return end
    if not startGuard() then return end
    startWall, startSim, startGame = now, core.getSimulationTime(), core.getGameTime()
    frame, checkpoint, lastControls, lastCell, lastPaused, lastSample = 0, 0, nil, nil, nil, -math.huge
    lastControlSequence = nil
    durationWarnings = {}
    recorder:start({ route_name = options.route_name, pose = pose(),
        simulation_origin = startSim, game_origin = startGame, real_origin = startWall,
        observer = observer, policy = policySummary(), policy_counts = policyCounts,
        private_lua_key_bindings = privateBindings,
        private_camera_setup = privateCamera,
        movement_cohort = options.movement_cohort, capture_scope = captureScope(), physical_state = physicalState(),
        resolved_controls = controlSnapshot().controls,
        checkpoint_save = checkpointSave, checkpoint_pose = checkpointPose }, relativeWall(now))
    bridge:set('last_immediate_sequence', recorder.seq)
    local suppressedKeys = {}
    for id in pairs(suppressedBeforeStart) do suppressedKeys[#suppressedKeys + 1] = id end
    table.sort(suppressedKeys)
    for _, id in ipairs(suppressedKeys) do
        local item = suppressedBeforeStart[id]
        local copy = {}
        for key, value in pairs(item) do copy[key] = value end
        copy.policy_event, copy.before_start = 'actor_suppressed', true
        push('policy', copy, now)
        suppressedLogged[item.reference_id] = true
    end
    core.sendGlobalEvent('OptimizedMWRouteRecording', { active = true, session = options.session, t_wall = now })
    message('Recording. Hold the fixed view, then walk your route. ' .. keyLabel('key_checkpoint', 'F9')
        .. ' marks a stage; ' .. keyLabel('key_finish', 'F10') .. ' finishes.')
end

local function start(now)
    if recorder.state ~= 'ready' or checkpointRequest then return end
    if paused() or not self.cell then message('Close menus before starting.'); return end
    if not startGuard() then return end
    if not observer.ready or not policy.ready or not policy.qualified or policy.failed then
        message('Waiting for door observation and confirmed hostile suppression.'); return
    end
    if self.controls.movement ~= 0 or self.controls.sideMovement ~= 0 or self.controls.jump then
        message('Stand still, release movement keys, then press ' .. keyLabel('key_start', 'F6') .. '.'); return
    end
    if not I.Controls or not I.Controls.overrideMovementControls then
        recorder.flags.checkpoint_failed = true
        finish('movement_setup_interface_unavailable', now)
        return
    end
    if not preparePrivateCamera() then
        recorder.flags.vanity_setup_failed = true
        finish('private_camera_setup_failed', now)
        return
    end
    if checkpointAttempts >= 8 then
        recorder.flags.checkpoint_failed = true
        finish('route_checkpoint_retry_budget_exceeded', now)
        return
    end
    checkpointPose = pose()
    checkpointWaitWall = now
    checkpointAttempts = checkpointAttempts + 1
    -- The MENU helper handles each request only once. A recoverable state change
    -- during save acknowledgement needs a fresh request and private save slot.
    checkpointRequest = options.session .. ':start' .. (checkpointAttempts > 1 and ':' .. checkpointAttempts or '')
    I.Controls.overrideMovementControls(true)
    setupFrozen = true
    holdSetup()
    bridge:set('checkpoint_ack', nil)
    bridge:set('checkpoint_request', checkpointRequest)
    message('Saving the private route starting state. Stand still until Recording appears.')
end

local triggers = { 'Activate', 'Jump', 'AutoMove', 'AlwaysRun', 'ToggleSneak', 'ToggleWeapon',
    'ToggleSpell', 'Inventory', 'Journal', 'GameMenu', 'QuickSave', 'QuickLoad', 'TogglePOV' }
local resolvedMovementTriggers = { AutoMove = true, AlwaysRun = true, ToggleSneak = true }
local function registerTriggers()
    if registeredTriggers then return end
    registeredTriggers = true
    for _, name in ipairs(triggers) do
        if input.triggers[name] then
            input.registerTriggerHandler(name, async:callback(function()
                if recorder.state == 'recording' and (not playbackCompatible or not closing) then
                    local now = core.getRealTime()
                    push('trigger', { name = name, f = frame,
                        simulation = core.getSimulationTime() - startSim,
                        playback_replayed_as = playbackCompatible and resolvedMovementTriggers[name] and 'resolved_native_control_snapshot' or nil,
                        observation = 'resolved_input_trigger; activation_success_not_implied' }, now)
                    if playbackCompatible and not resolvedMovementTriggers[name] then
                        recorder.flags.playback_scope_invalid = true
                        finish('unsupported_input_trigger_' .. name:lower(), now)
                        return
                    end
                    if name == 'Activate' then
                        activationCounts.input_attempts = activationCounts.input_attempts + 1
                        local ok, target = pcall(function()
                            local from = camera.getPosition()
                            local reach = core.getGMST('iMaxActivateDist') + camera.getThirdPersonDistance()
                            local destination = from + camera.viewportToWorldVector(util.vector2(0.5, 0.5)) * reach
                            local ray = nearby.castRenderingRay(from, destination, { ignore = self.object })
                            local object = ray.hitObject
                            local snapshot = { name = 'ActivateTargetSnapshot', source = 'synchronous_center_screen_render_ray',
                                actual_request = false, f = frame, attempt = activationCounts.input_attempts,
                                hit = ray.hit, observed = ray.hit and object ~= nil,
                                position = vector(from), hit_position = ray.hitPos and vector(ray.hitPos) or nil,
                                player_pose = pose(), source_wall = now, source_simulation = core.getSimulationTime() }
                            if object then
                                local z, y, x = object.rotation:getAnglesZYX()
                                snapshot.target = { id = tostring(object.id), record_id = object.recordId,
                                    position = vector(object.position), rotation = { z = z, y = y, x = x }, cell = cellSnapshot(object.cell) }
                            end
                            return snapshot
                        end)
                        if not ok then target = { name = 'ActivateTargetSnapshot', source = 'synchronous_center_screen_render_ray', actual_request = false,
                            observed = false, error = tostring(target):sub(1, 512), f = frame,
                            attempt = activationCounts.input_attempts, source_wall = now } end
                        if target.observed then activationCounts.visual_known = activationCounts.visual_known + 1
                        else activationCounts.visual_unknown = activationCounts.visual_unknown + 1 end
                        push('trigger', target, now)
                    end
                    if name == 'QuickLoad' then recorder.flags.reloaded = true; finish('quickload_during_recording', now) end
                end
            end))
        end
    end
end

local function onFrame()
    if not options.enabled then return end
    local now = core.getRealTime()
    if recorder.state == 'exporting' then
        if recorder:flush(32) then
            message('Export complete.')
            if options.quit_on_finish or (options.smoke_autostart and options.smoke_quit) then core.quit() end
        end
        return
    elseif recorder.state == 'finished' then return end
    if reenteredSession then
        reenteredSession = false
        wallOrigin = bridge:get('ready_real_origin') or now
        recorder.seq = bridge:get('last_immediate_sequence') or 0
        recorder.flags.reloaded, recorder.flags.lost_buffer = true, true
        finalize('session_reentered_after_load', now)
        return
    end
    preparePrivateLuaBindings()
    registerTriggers()
    if closing then
        closing.frames = closing.frames + 1
        local drained = closing.acknowledged and (policy.pending_disable or 0) == 0
        if playbackCompatible then drained = drained and policy.pending_disable == 0 end
        if drained and closing.frames >= 4 then
            finalize(closing.reason, now)
        elseif now - closing.end_wall > 5 then
            recorder.flags.observation_settle_timeout = true
            recorder.flags.combat_invalid = true
            finalize('policy_finish_ack_timeout', now)
        end
        return
    end
    if checkpointRequest then
        holdSetup()
        local ack = bridge:getCopy('checkpoint_ack')
        if ack and ack.request == checkpointRequest then
            if not ack.ok then
                recorder.flags.checkpoint_failed = true
                recorder.flags.checkpoint_error = ack.error
                finish('route_checkpoint_save_failed', now)
                return
            end
            checkpointSave = ack.save
            checkpointRequest = nil
            releaseSetup()
            beginRecording(now)
        elseif now - checkpointWaitWall > 20 then
            recorder.flags.checkpoint_failed = true
            finish('route_checkpoint_save_timeout', now)
            return
        else return end
    end
    if recorder.state == 'waiting' then
        local cell = self.cell
        local cellKey = cell and common.json(cellSnapshot(cell)) or nil
        local expected = options.smoke_autostart and options.smoke_expected_cell
        local expectedMatch = not expected or (cell and cell.isExterior and cell.name:lower():find(expected:lower(), 1, true))
        if paused() or not cellKey or not expectedMatch then stableSince, stableCell = nil, nil; return end
        if stableCell ~= cellKey then stableCell, stableSince = cellKey, now; return end
        if now - stableSince < 2 then return end
        if not preparePrivateCamera() then
            recorder.flags.vanity_setup_failed = true
            finish('private_camera_setup_failed', now)
            return
        end
        wallOrigin = now
        recorder:ready({ route_name = options.route_name, sample_hz = options.sample_hz,
            limits = { seconds = options.max_seconds, wall_seconds = maxWallSeconds,
                records = options.max_records, bytes = options.max_bytes },
            keys = { start = options.key_start or 'F6', checkpoint = options.key_checkpoint or 'F9',
                finish = options.key_finish or 'F10', abort = options.key_abort or 'F11', require_ctrl_shift = options.require_ctrl_shift == true },
            observer = observer, policy = policySummary(),
            private_lua_key_bindings = privateBindings,
            private_camera_setup = privateCamera,
            movement_cohort = options.movement_cohort, capture_scope = captureScope(), physical_state = physicalState(),
            control_phase = 'player_onFrame_after_earlier_content_handlers; native_pitch_yaw_readout_not_post_physics',
            capture_purpose = 'route_acquisition; dt_real_not_independent_benchmark_frame_measurement',
            pose = pose() }, 0)
        bridge:set('ready_written', true)
        bridge:set('ready_real_origin', wallOrigin)
        bridge:set('last_immediate_sequence', recorder.seq)
        message('Ready. ' .. keyLabel('key_start', 'F6') .. ' starts; ' .. keyLabel('key_checkpoint', 'F9')
            .. ' marks; ' .. keyLabel('key_finish', 'F10') .. ' finishes; ' .. keyLabel('key_abort', 'F11') .. ' aborts.')
    end
    if options.smoke_autostart and recorder.state == 'ready' then pendingCommand = 'start' end
    if pendingCommand then
        local command = pendingCommand
        pendingCommand = nil
        if command == 'start' then start(now)
        elseif command == 'finish' then finish('user_finished', now)
        elseif command == 'abort' then finish('user_aborted', now)
        elseif command == 'checkpoint' then
            if recorder.state == 'recording' then
                checkpoint = checkpoint + 1
                push('checkpoint', { index = checkpoint, f = frame, pose = pose() }, now)
                message('Stage marker ' .. checkpoint .. ' recorded.')
            elseif recorder.state == 'ready' then
                message('Not recording. Press ' .. keyLabel('key_start', 'F6') .. ' to start first; '
                    .. keyLabel('key_checkpoint', 'F9') .. ' adds a stage marker after recording starts.')
            end
        end
    end
    if recorder.state ~= 'recording' then return end
    frame = frame + 1
    local budgetElapsed = playbackCompatible and core.getSimulationTime() - startSim or now - startWall
    if budgetElapsed >= options.max_seconds then finish('maximum_duration_reached', now); return end
    if playbackCompatible and now - startWall >= maxWallSeconds then finish('maximum_wall_duration_reached', now); return end
    if playbackCompatible then
        local remaining = options.max_seconds - budgetElapsed
        for _, threshold in ipairs({ 30, 10 }) do
            if remaining <= threshold and not durationWarnings[threshold] then
                durationWarnings[threshold] = true
                message('Less than ' .. threshold .. ' simulation seconds remain. Press ' .. keyLabel('key_finish', 'F10')
                    .. ' to finish; reaching the ' .. options.max_seconds .. '-simulation-second cap is incomplete.')
            end
        end
        local failure = scopeFailure()
        if failure then recorder.flags.playback_scope_invalid = true; finish('playback_scope_' .. failure, now); return end
    end
    if options.smoke_autostart and now - startWall >= options.smoke_duration_seconds then
        finish('smoke_finished', now); return
    end
    local isPaused = paused()
    if isPaused ~= lastPaused then
        push('pause', { paused = isPaused, f = frame,
            simulation = core.getSimulationTime() - startSim, ui_mode = I.UI and I.UI.getMode() or nil }, now)
        lastPaused = isPaused
        if isPaused then recorder.flags.contains_pauses = true end
        if isPaused and playbackCompatible then recorder.flags.playback_scope_invalid = true; finish('playback_scope_paused', now); return end
    end
    if not self.cell then
        if not recorder.flags.cell_unavailable_seen then push('cell', { unavailable = true, f = frame }, now) end
        recorder.flags.cell_unavailable_seen = true
        return
    end
    local cell = cellSnapshot(self.cell)
    local cellKey = common.json(cell)
    if cellKey ~= lastCell then push('cell', { cell = cell, f = frame }, now); lastCell = cellKey end
    local controls = controlSnapshot()
    local encoded = common.json(controls)
    if encoded ~= lastControls then
        local clock = clocks(now)
        controls.f, controls.simulation, controls.game, controls.dt_real = clock.f, clock.simulation, clock.game, clock.dt_real
        push('control', controls, now)
        lastControlSequence = recorder.seq
        lastControls = encoded
    end
    if playbackCompatible and (controls.controls.jump or controls.controls.use ~= 0 or controls.actions.use) then
        recorder.flags.playback_scope_invalid = true
        finish('unsupported_jump_or_combat_input', now)
        return
    end
    if now - lastSample >= 1 / options.sample_hz then
        local sample = clocks(now)
        local p = pose()
        for key, value in pairs(p) do sample[key] = value end
        sample.paused = isPaused
        sample.control_sequence = lastControlSequence
        push('sample', sample, now)
        lastSample = now -- no synthetic catch-up samples after a hitch
    end
end

local function guardedFrame()
    local ok, failure = pcall(onFrame)
    if not ok then
        recorder.flags.nonfinite_data = true
        recorder.flags.error = tostring(failure):sub(1, 512)
        if recorder.state ~= 'exporting' and recorder.state ~= 'finished' then
            finish('recorder_error', core.getRealTime())
        end
    end
end

local function onKeyPress(e)
    if not options.enabled or recorder.state == 'exporting' or recorder.state == 'finished' then return end
    if options.require_ctrl_shift == true then
        if not (e.withCtrl and e.withShift) or e.withAlt or e.withSuper then return end
    elseif e.withCtrl or e.withShift or e.withAlt or e.withSuper then return end
    local bindings = {
        [input.KEY[options.key_start or 'F6']] = 'start', [input.KEY[options.key_checkpoint or 'F9']] = 'checkpoint',
        [input.KEY[options.key_finish or 'F10']] = 'finish', [input.KEY[options.key_abort or 'F11']] = 'abort',
    }
    local command = bindings[e.code]
    if command == 'abort' then
        pendingCommand = nil
        finish('user_aborted', core.getRealTime())
        return
    end
    if recorder.state ~= 'waiting' then pendingCommand = command or pendingCommand end
end

return {
    engineHandlers = {
        onInit = function() preparePrivateLuaBindings() end,
        onFrame = guardedFrame,
        onKeyPress = onKeyPress,
        onLoad = function()
            if recorder.state == 'waiting' then preparePrivateLuaBindings() end
            if recorder.state ~= 'waiting' and recorder.state ~= 'finished' and recorder.state ~= 'exporting' then
                recorder.flags.reloaded = true
                finish('save_loaded_after_recorder_ready', core.getRealTime())
            end
        end,
        onSave = function() return { schema = 1, recording_saved = recorder.state == 'recording' } end,
    },
    eventHandlers = {
        OptimizedMWRouteObserver = function(data) observer = data end,
        OptimizedMWRouteDoor = function(data)
            if recorder.state == 'recording' then
                data.f = frame; push('door', data, core.getRealTime())
                if playbackCompatible then recorder.flags.playback_scope_invalid = true; finish('unsupported_door_interaction', core.getRealTime()) end
            end
        end,
        OptimizedMWRouteActivation = function(data)
            if recorder.state == 'recording' then
                data.f, data.actual_request = frame, true
                if data.observed then activationCounts.actual_known = activationCounts.actual_known + 1
                else
                    activationCounts.actual_unknown = activationCounts.actual_unknown + 1
                    recorder.flags.activation_target_unresolved = true
                end
                push('activation', data, core.getRealTime())
                if playbackCompatible then recorder.flags.playback_scope_invalid = true; finish('unsupported_activation', core.getRealTime()) end
            end
        end,
        OptimizedMWRoutePolicy = function(data)
            if playbackCompatible and (type(data) ~= 'table' or data.session ~= options.session) then return end
            if data.event == 'policy_ready' then policy = data end
            if data.event == 'policy_stopped' then
                policy = data
                if closing then closing.acknowledged = true end
            end
            if recorder.state == 'recording' then
                if data.event == 'combat' then
                    local observed = data.observed_t_wall
                    if not observed or (observed >= startWall and (not closing or observed <= closing.end_wall)) then
                        recorder.flags.combat_invalid = true
                    end
                end
                local payload = (data.event == 'policy_ready' or data.event == 'policy_stopped') and policySummary() or data
                payload.policy_event = data.event
                if data.event ~= 'actor_suppressed' or not suppressedLogged[data.reference_id] then
                    push('policy', payload, core.getRealTime())
                    if data.event == 'actor_suppressed' then suppressedLogged[data.reference_id] = true end
                end
            elseif data.event == 'actor_suppressed' then
                if not suppressedBeforeStart[data.reference_id] then
                    if policyCounts.suppressed_before_start >= 4096 then
                        recorder.flags.budget_exceeded = true
                        finish('initial_hostile_manifest_exceeded', core.getRealTime())
                        return
                    end
                    policyCounts.suppressed_before_start = policyCounts.suppressed_before_start + 1
                    suppressedBeforeStart[data.reference_id] = data
                end
            elseif data.event == 'combat' then policyCounts.combat_before_start = policyCounts.combat_before_start + 1 end
        end,
        OptimizedMWRouteCommand = function(data)
            -- Private console-driven smoke/integration tests only. Not a replay API.
            if options.allow_test_events and type(data) == 'table' then
                if data.command == 'start' or data.command == 'checkpoint' or data.command == 'finish' or data.command == 'abort' then
                    pendingCommand = data.command
                end
            end
        end,
    },
}
