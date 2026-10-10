-- Private route preview. Last in content order; physics consumes movement inputs.
-- Camera sampling and position-error observation do not make this a benchmark.
local core = require('openmw.core')
local self = require('openmw.self')
local camera = require('openmw.camera')
local input = require('openmw.input')
local ui = require('openmw.ui')
local types = require('openmw.types')
local I = require('openmw.interfaces')
local sample = require('scripts.optimizedmw_route_playback.core')
local guidance = require('scripts.optimizedmw_route_playback.guidance')
local options = require('scripts.optimizedmw_route_playback.options')
local timeline = require('scripts.optimizedmw_route_playback.timeline')
local common = require('scripts.optimizedmw_route_recorder.common')
local layeredCamera = sample.cameraLayers(timeline.camera_layer_cohort)
local cameraLayerStatus = layeredCamera and 'recorded_additive' or 'legacy_zero_layers'
local cameraUtil = layeredCamera and require('openmw.util') or nil
local layerAngles = {'extra_yaw','extra_pitch','roll','extra_roll'}
local layerGetters = {extra_yaw='getExtraYaw',extra_pitch='getExtraPitch',roll='getRoll',extra_roll='getExtraRoll'}
local layerSetters = {extra_yaw='setExtraYaw',extra_pitch='setExtraPitch',roll='setRoll',extra_roll='setExtraRoll'}
-- Lua 5.1 caps a function at 60 upvalues. Group the camera contract's state and
-- callbacks so the existing frame state machine remains compatible.
local cameraLayer = {active=layeredCamera,status=cameraLayerStatus,angles=layerAngles,
    restored_failed=false,maximum_angle_error=0,maximum_offset_error=0}
if layeredCamera then
    assert(sample.cameraLayers(options.camera_layer_cohort), 'Missing playback camera layer cohort')
else
    assert(options.camera_layer_cohort == nil, 'Playback options/timeline camera cohort mismatch')
end
local debugAvailable, debugApi = pcall(require, 'openmw.debug')
if not debugAvailable then debugApi = nil end
assert(options.enabled == true and type(options.session) == 'string' and options.session:match('^[0-9a-f]+$') and #options.session == 32,
    'Invalid private playback session')
local function bounded(name, default, minimum, maximum)
    local value = options[name] or default
    assert(sample.finite(value) and value >= minimum and value <= maximum, 'Invalid playback option: ' .. name)
    return value
end
local autostart = bounded('autostart_seconds', 3, 1, 10)
local setupTimeout = bounded('setup_timeout_seconds', 90, 5, 180)
local initialTolerance = bounded('position_tolerance', 128, 1, 512)
local abortTolerance = bounded('abort_position_error', 2048, initialTolerance, 8192)
local maximumDuration = bounded('max_duration_seconds', 180, 1, 180)
assert(timeline.duration <= maximumDuration, 'Route exceeds playback limit')
assert(options.guidance_enabled == nil or type(options.guidance_enabled) == 'boolean', 'Invalid guidance option')
local guided = options.guidance_enabled == true
local protocol = guided and guidance.PROTOCOL or 'raw_physics_input'
local guideClock = guided and guidance.newClock(timeline.duration) or nil
local stallGuard = guided and guidance.newStallGuard() or nil
local alignOrigin, aligned, setupLastCheck = nil, false, -math.huge
local initialSeed, maximumHorizontalError, maximumVerticalError = nil, 0, 0
local lastReference, lastGuidanceCommand = 0, nil
local lookup = sample.newSampler(timeline)
local commandLookup = sample.newCommandSampler(timeline)
local state, wallOrigin, simOrigin, readyElapsed, setupOrigin = 'waiting', nil, nil, 0, nil
local policy = { ready = false }
local stopAcknowledged, closeWall, closeFrames, finalReason, finishPrinted = false, nil, 0, nil, false
local lastCheck, markerIndex, sequence, checkCount, maximumError, cameraError = -math.huge, 1, 0, 0, 0, 0
local pitchError, cellMismatchCount, outsideToleranceCount = 0, 0, 0
local postwriteYawError, postwritePitchError = 0, 0
local overrides = false
local initialCameraLayers, cameraLayersOwned = nil, false
local function emit(event, payload)
    payload = payload or {}
    sequence = sequence + 1
    payload.schema, payload.session, payload.event, payload.seq = 1, options.session, event, sequence
    payload.playback_protocol = protocol
    payload.camera_layer_cohort = timeline.camera_layer_cohort
    payload.camera_layer_cohort_status = cameraLayerStatus
    payload.t_simulation = simOrigin and math.max(0, core.getSimulationTime() - simOrigin) or 0
    payload.t_wall = wallOrigin and math.max(0, core.getRealTime() - wallOrigin) or 0
    print('OMWPLAY|' .. common.json(payload))
end
local function cameraLayerSnapshot(readback)
    local result = {}
    for _, key in ipairs(layerAngles) do result[key] = camera[layerGetters[key]]() end
    local offset = camera.getFirstPersonOffset()
    result.first_person_offset = {x=offset.x,y=offset.y,z=offset.z}
    return sample.validateCameraLayers(result,readback == true)
end
local function prepareCameraLayers()
    if not layeredCamera or initialCameraLayers then return end
    for _, key in ipairs(layerAngles) do
        assert(type(camera[layerGetters[key]]) == 'function' and type(camera[layerSetters[key]]) == 'function',
            'Required additive camera API unavailable: '..key)
    end
    assert(type(camera.getFirstPersonOffset) == 'function' and type(camera.setFirstPersonOffset) == 'function'
        and type(cameraUtil.vector3) == 'function', 'Required first-person camera offset API unavailable')
    -- Capture ownership before any override or camera write. Release restores
    -- only these private camera channels, without touching player storage.
    initialCameraLayers = cameraLayerSnapshot()
end
local function writeCameraLayers(value)
    cameraLayersOwned = true -- a partial setter failure must still restore ownership
    for _, key in ipairs(layerAngles) do camera[layerSetters[key]](value[key]) end
    local offset = value.first_person_offset
    camera.setFirstPersonOffset(cameraUtil.vector3(offset.x,offset.y,offset.z))
    -- Verify every commanded frame, including setup, even when check output is
    -- throttled. A silent setter mismatch must not pass between diagnostics.
    local actual = cameraLayerSnapshot(true)
    local errors, maximum = {}, 0
    for _, key in ipairs(layerAngles) do
        errors[key] = math.abs(value[key]-actual[key])
        maximum = math.max(maximum,errors[key])
    end
    local offsetError = sample.distance(value.first_person_offset,actual.first_person_offset)
    cameraLayer.maximum_angle_error = math.max(cameraLayer.maximum_angle_error,maximum)
    cameraLayer.maximum_offset_error = math.max(cameraLayer.maximum_offset_error,offsetError)
    cameraLayer.readback = {actual=actual,errors=errors,offset_error=offsetError}
    assert(maximum <= 0.00001 and offsetError <= 0.001, 'Additive camera setter readback mismatch')
end
local function restoreCameraLayers()
    if not cameraLayersOwned then return end
    local errors = {}
    for _, key in ipairs(layerAngles) do
        local ok, failure = pcall(camera[layerSetters[key]],initialCameraLayers[key])
        if not ok then errors[#errors+1] = key..': '..tostring(failure):sub(1,128) end
    end
    local offset = initialCameraLayers.first_person_offset
    local ok, failure = pcall(function() camera.setFirstPersonOffset(cameraUtil.vector3(offset.x,offset.y,offset.z)) end)
    if not ok then errors[#errors+1] = 'first_person_offset: '..tostring(failure):sub(1,128) end
    if #errors == 0 then
        local readOk, actual = pcall(cameraLayerSnapshot,true)
        if not readOk then errors[#errors+1] = 'restoration readback: '..tostring(actual):sub(1,128)
        else
            for _, key in ipairs(layerAngles) do
                if math.abs(initialCameraLayers[key]-actual[key]) > 0.00001 then errors[#errors+1] = key..': restoration readback mismatch' end
            end
            if sample.distance(initialCameraLayers.first_person_offset,actual.first_person_offset) > 0.001 then
                errors[#errors+1] = 'first_person_offset: restoration readback mismatch'
            end
        end
    end
    if #errors > 0 then
        cameraLayer.restored_failed = true
        emit('error',{message='Private camera layer restoration failed',camera_restore_errors=errors})
    else
        cameraLayersOwned = false
        cameraLayer.restored = true
    end
end
cameraLayer.prepare, cameraLayer.snapshot = prepareCameraLayers, cameraLayerSnapshot
function cameraLayer.annotate(observation,commandCamera)
    if not cameraLayer.active then return end
    local readback = cameraLayer.readback
    observation.command_camera_layers = {}
    for _, key in ipairs(layerAngles) do observation.command_camera_layers[key] = commandCamera[key] end
    observation.command_camera_layers.first_person_offset = commandCamera.first_person_offset
    observation.camera_layers_after_write, observation.camera_layer_postwrite_errors = readback.actual, readback.errors
    observation.first_person_offset_postwrite_error = readback.offset_error
    observation.camera_layer_observation_scope = 'sampled additive channels after setters; not final rendered bone camera'
end
local function zero()
    self.controls.movement, self.controls.sideMovement = 0, 0
    self.controls.jump, self.controls.sneak = false, false
    self.controls.use = self.ATTACK_TYPE.NoAttack
    self.controls.yawChange, self.controls.pitchChange = 0, 0
end
local function release()
    zero()
    if overrides then
        I.Controls.overrideMovementControls(false)
        I.Controls.overrideCombatControls(false)
        I.Controls.overrideUiControls(false)
        overrides = false
    end
    restoreCameraLayers()
end
local function cameraPose(pose)
    camera.setMode(camera.MODE.FirstPerson, true)
    camera.setYaw(pose.camera.yaw)
    camera.setPitch(pose.camera.pitch)
    camera.setFieldOfView(pose.camera.fov)
    if layeredCamera then writeCameraLayers(pose.camera) end
    -- Absolute camera setters do not rotate the actor. Native mechanics consumes
    -- these actor-angle deltas; they are never accumulated from recorded FPS.
    self.controls.yawChange = sample.angleDelta(pose.camera.yaw, self.rotation:getYaw())
    self.controls.pitchChange = pose.camera.pitch - self.rotation:getPitch()
end
local function close(reason)
    if state == 'closing' or state == 'finished' then return end
    finalReason, state, closeWall, closeFrames = reason, 'closing', core.getRealTime(), 0
    zero()
    core.sendGlobalEvent('OptimizedMWRouteRecording', { active = false, session = options.session, t_wall = closeWall })
end
local function paused() return core.isWorldPaused() or (I.UI and I.UI.getMode() ~= nil) end
local function currentPosition() return { x = self.position.x, y = self.position.y, z = self.position.z } end
local function safeRead(fn, object)
    if type(fn) ~= 'function' then return nil end
    local ok, value
    if object == nil then ok, value = pcall(fn) else ok, value = pcall(fn, object) end
    if ok and (type(value) == 'boolean' or sample.finite(value)) then return value end
    return nil
end
local function physicalState()
    local actor = types.Actor or {}
    local collision
    if debugApi then collision = safeRead(debugApi.isCollisionEnabled) end
    return { collision_enabled = collision,
        on_ground = safeRead(actor.isOnGround, self), swimming = safeRead(actor.isSwimming, self),
        current_speed = safeRead(actor.getCurrentSpeed, self) }
end
local function gaitSpeed(run)
    assert(types.Actor, 'Actor speed API unavailable')
    local value = safeRead(run and types.Actor.getRunSpeed or types.Actor.getWalkSpeed, self)
    assert(sample.finite(value) and value > 0, 'Actor gait speed unavailable or nonpositive')
    return value
end
local function inspect(expected, t, deferEmission)
    local position = currentPosition()
    local error = sample.distance(position, expected.position)
    maximumError = math.max(maximumError, error)
    local horizontalError, verticalError = guidance.errors(position, expected.position)
    maximumHorizontalError = math.max(maximumHorizontalError, horizontalError)
    maximumVerticalError = math.max(maximumVerticalError, verticalError)
    local angle = math.abs(sample.angleDelta(expected.camera.yaw, camera.getYaw()))
    cameraError = math.max(cameraError, angle)
    local pitch = math.abs(expected.camera.pitch - camera.getPitch())
    pitchError = math.max(pitchError, pitch)
    local cell = self.cell
    if not cell or cell.id ~= expected.cell_id then cellMismatchCount = cellMismatchCount + 1 end
    if error > initialTolerance then outsideToleranceCount = outsideToleranceCount + 1 end
    local observation = { expected_time = t, actual_position = position, expected_position = expected.position,
        position_error = error, expected_cell = expected.cell_id, actual_cell = cell and cell.id or nil,
        horizontal_error = horizontalError, vertical_error = verticalError, physical_state = physicalState(),
        cell_matches = cell ~= nil and cell.id == expected.cell_id, expected_camera_yaw = expected.camera.yaw,
        actual_camera_yaw = camera.getYaw(), camera_yaw_error = angle, expected_camera_pitch = expected.camera.pitch,
        actual_camera_pitch = camera.getPitch(), camera_pitch_error = pitch, sample_interval = expected.source_interval,
        actual_actor_yaw_before = self.rotation:getYaw(), actual_actor_pitch_before = self.rotation:getPitch(),
        expected_current_actor_yaw_from_camera = expected.camera.yaw,
        expected_current_actor_pitch_from_camera = expected.camera.pitch,
        camera_observation_phase = 'before_current_frame_commands',
        expected_actor_heading_source = 'recorded_first_person_camera; actor_XZ_not_separately_acquired' }
    if not deferEmission then emit('check', observation) end
    checkCount = checkCount + 1
    if guided then
        if horizontalError > guidance.limits.hard_xy then close('guided_horizontal_divergence')
        elseif verticalError > guidance.limits.hard_z then close('guided_vertical_divergence')
        elseif observation.physical_state.collision_enabled ~= true then
            close(observation.physical_state.collision_enabled == false and 'collision_disabled' or 'collision_state_unavailable') end
    elseif error > abortTolerance then close('position_divergence') end
    return observation
end
local function drainClosing(now)
        zero()
        closeFrames = closeFrames + 1
        local drained = stopAcknowledged and (policy.pending_disable or 0) == 0
        local timedOut = now - closeWall >= 5
        if (drained and closeFrames >= 4 and now - closeWall >= 1) or timedOut then
            local finalPhysics = physicalState()
            if finalReason == 'route_finished' then
                if not stopAcknowledged then finalReason = 'observer_drain_timeout'
                elseif (policy.pending_disable or 0) > 0 then finalReason = 'hostile_policy_pending_at_exit'
                elseif policy.failed or (policy.failure_count or 0) > 0 then finalReason = 'hostile_policy_failed'
                elseif policy.qualified ~= true then finalReason = 'hostile_policy_unqualified_at_exit'
                elseif guided and finalPhysics.collision_enabled ~= true then
                    finalReason = finalPhysics.collision_enabled == false and 'collision_disabled' or 'collision_state_unavailable' end
            end
            release()
            if cameraLayer.restored_failed and finalReason == 'route_finished' then finalReason = 'camera_restore_failed' end
            state = 'finished'
            if not finishPrinted then
                finishPrinted = true
                emit('finish', { reason = finalReason, route_playback_finished = finalReason == 'route_finished',
                    input_replay_finished = not guided and finalReason == 'route_finished',
                    benchmark_qualified = false, qualification = 'PREVIEW_NOT_BENCHMARK_VALIDATED',
                    maximum_position_error = maximumError, maximum_camera_yaw_error = cameraError,
                    maximum_horizontal_error = maximumHorizontalError, maximum_vertical_error = maximumVerticalError,
                    maximum_camera_pitch_error = pitchError, cell_mismatch_check_count = cellMismatchCount,
                    maximum_camera_postwrite_yaw_error = postwriteYawError,
                    maximum_camera_postwrite_pitch_error = postwritePitchError,
                    maximum_camera_layer_postwrite_error = cameraLayer.maximum_angle_error,
                    maximum_first_person_offset_postwrite_error = cameraLayer.maximum_offset_error,
                    camera_layers_restored = cameraLayer.active and cameraLayer.restored == true,
                    camera_layer_restoration_failed = cameraLayer.restored_failed,
                    outside_position_tolerance_check_count = outsideToleranceCount,
                    reference_time = lastReference, source_elapsed = simOrigin and core.getSimulationTime() - simOrigin or 0,
                    reference_hold_count = guideClock and guideClock.hold_count or 0,
                    reference_hold_seconds = guideClock and guideClock.hold_seconds or 0,
                    reference_clock_lag = guideClock and guideClock.source - guideClock.reference or 0,
                    final_actual_position = currentPosition(), physical_state = finalPhysics,
                    last_guidance_command = lastGuidanceCommand,
                    check_count = checkCount, policy = policy, policy_stop_acknowledged = stopAcknowledged,
                    policy_observer_drained = drained, policy_drain_timed_out = timedOut and not drained })
            end
            if options.quit_on_finish then core.quit() end
        end
end
local function frame(dt)
    local now = core.getRealTime()
    if state == 'finished' then return end
    if state == 'closing' then drainClosing(now); return end
    if not setupOrigin then
        setupOrigin = now
        cameraLayer.prepare()
        assert(I.Controls and I.Controls.overrideMovementControls and I.Controls.overrideCombatControls and I.Controls.overrideUiControls,
            'Controls override interface unavailable')
        I.Controls.overrideMovementControls(true)
        I.Controls.overrideCombatControls(true)
        overrides = true
        types.Player.setControlSwitch(self, types.Player.CONTROL_SWITCH.VanityMode, false)
        emit('setup', { scope = 'private_physics_route_preview', route = timeline.route_name, teleport_writes = 0,
            physical_state = physicalState(), guidance_limits = guided and guidance.limits or nil })
    end
    if state == 'waiting' then
        zero()
        if now - setupOrigin >= setupTimeout then close('setup_timeout'); return end
        if paused() or not self.cell then readyElapsed = 0; return end
        if policy.failed then close('hostile_policy_failed'); return end
        if not policy.ready or not policy.qualified or (policy.pending_disable or 0) ~= 0 then readyElapsed = 0; return end
        local initial = timeline.samples[1]
        if self.cell.id ~= initial.cell_id or not self.cell.isExterior then close('starting_cell_mismatch'); return end
        if sample.distance(currentPosition(), initial.position) > initialTolerance then close('starting_position_mismatch'); return end
        cameraPose(initial)
        if guided then
            local physics = physicalState()
            if physics.collision_enabled ~= true then
                close(physics.collision_enabled == false and 'collision_disabled' or 'collision_state_unavailable'); return end
            local xy, z = guidance.errors(currentPosition(), initial.position)
            if z > guidance.limits.setup_z then close('initial_vertical_mismatch'); return end
            if not initialSeed then initialSeed = currentPosition() end
            if not aligned then
                if not alignOrigin then alignOrigin = core.getSimulationTime() end
                if xy <= guidance.limits.setup_xy then
                    aligned = true
                    emit('alignment_finished', { initial_seed_position = initialSeed, actual_position = currentPosition(),
                        expected_position = initial.position, horizontal_error = xy, vertical_error = z,
                        alignment_seconds = core.getSimulationTime() - alignOrigin, physical_state = physics })
                else
                    if core.getSimulationTime() - alignOrigin >= guidance.limits.setup_seconds then close('initial_alignment_timeout'); return end
                    local alignment = guidance.setupCommand(currentPosition(), initial.position, initial.camera.yaw, gaitSpeed(false))
                    self.controls.movement, self.controls.sideMovement = alignment.movement, alignment.side_movement
                    self.controls.run, self.controls.sneak = false, false
                    if core.getSimulationTime() - setupLastCheck >= 0.2 then
                        emit('alignment_check', { actual_position = currentPosition(), expected_position = initial.position,
                            command = alignment, physical_state = physics, elapsed = core.getSimulationTime() - alignOrigin })
                        setupLastCheck = core.getSimulationTime()
                    end
                    readyElapsed = 0
                    return
                end
            end
            -- Do not let the fixed-view setup drift after alignment was accepted.
            if xy > guidance.limits.setup_xy then aligned, readyElapsed = false, 0; return end
        end
        readyElapsed = readyElapsed + math.max(0, dt or 0)
        if readyElapsed < autostart then return end
        I.Controls.overrideUiControls(true)
        wallOrigin, simOrigin, state = now, core.getSimulationTime(), 'playing'
        core.sendGlobalEvent('OptimizedMWRouteRecording', { active = true, session = options.session, t_wall = now })
        local routeLabel = timeline.route_name == 'existing' and 'Seyda Neen' or timeline.route_name
        ui.showMessage('Route playback (' .. routeLabel .. '): automatic preview. F11 aborts.')
        emit('start', { duration_simulation = timeline.duration, starting_position = currentPosition(),
            initial_position_tolerance = initialTolerance, divergence_abort = abortTolerance,
            policy = policy, phase = 'position_onFrame_before_physics; heading_command_at_simulation_plus_dt',
            heading_phase_schema = 2, benchmark_qualified = false,
            physical_state = physicalState(), guidance_limits = guided and guidance.limits or nil })
    end
    if paused() then zero(); close('unexpected_pause'); return end
    if policy.failed or (policy.failure_count or 0) > 0 then close('hostile_policy_failed'); return end
    local elapsed = math.max(0, core.getSimulationTime() - simOrigin)
    if elapsed > maximumDuration then close('duration_limit'); return end
    local reference, lag = elapsed, 0
    if guided then
        reference, lag = guideClock:advance(elapsed)
        if lag > guidance.limits.maximum_lag then close('reference_clock_lag'); return end
        local collision = physicalState().collision_enabled
        if collision ~= true then close(collision == false and 'collision_disabled' or 'collision_state_unavailable'); return end
    end
    lastReference = math.min(reference, timeline.duration)
    if reference >= timeline.duration then
        local _, expected = lookup(timeline.duration)
        if cameraLayer.active then
            local terminal = inspect(expected,timeline.duration,true)
            if state == 'playing' then
                cameraPose(expected)
                cameraLayer.annotate(terminal,expected.camera)
                terminal.camera_layer_command_phase = 'terminal_reference_endpoint; before_release'
            end
            emit('check',terminal)
        else inspect(expected, timeline.duration) end
        if state == 'playing' and guided and not guidance.finalPoseAccepted(currentPosition(), expected.position) then close('final_pose_mismatch') end
        if state == 'playing' then close('route_finished') end
        return
    end
    local controls, expected = lookup(reference)
    local held, holdChanged = false, false
    if guided then
        local xy, z = guidance.errors(currentPosition(), expected.position)
        if xy > guidance.limits.hard_xy or z > guidance.limits.hard_z then
            inspect(expected, reference); return
        end
        held, holdChanged = guideClock:updateHold(xy)
        if held and controls.movement == 0 and controls.side_movement == 0 then close('stationary_pose_divergence'); return end
        if holdChanged then emit(held and 'reference_hold' or 'reference_resume',
            { reference_time = reference, source_elapsed = elapsed, reference_clock_lag = lag,
                horizontal_error = xy, vertical_error = z }) end
    end
    local advance = guided and guideClock:futureDelta(dt) or dt
    local command, commandTime = commandLookup(reference, advance)
    local observation
    if elapsed - lastCheck >= 0.2 then
        -- Observation is before this frame's new camera/actor inputs are applied.
        observation = inspect(expected, reference, true)
        lastCheck = elapsed
        if state ~= 'playing' then emit('check', observation); return end
    end
    self.controls.movement, self.controls.sideMovement = controls.movement, controls.side_movement
    self.controls.run, self.controls.sneak = controls.run, controls.sneak
    self.controls.jump, self.controls.use = false, self.ATTACK_TYPE.NoAttack
    -- Acquisition samples are pre-physics headings. Mechanics consumes the
    -- current rotation command within this frame, so target its end-of-frame
    -- heading while movement controls remain selected at current elapsed time.
    cameraPose(command)
    local applied
    if guided then
        applied = guidance.command(currentPosition(), command.position, controls, command.camera.yaw, gaitSpeed(controls.run), held)
        self.controls.movement, self.controls.sideMovement = applied.movement, applied.side_movement
        lastGuidanceCommand = applied
        if stallGuard:observe(elapsed, currentPosition(), applied.input_norm) then close('physical_movement_stalled') end
    end
    if observation then
        observation.command_time, observation.frame_simulation_delta = commandTime, dt
        observation.command_camera_yaw, observation.command_camera_pitch = command.camera.yaw, command.camera.pitch
        observation.command_actor_yaw_delta, observation.command_actor_pitch_delta = self.controls.yawChange, self.controls.pitchChange
        observation.source_elapsed, observation.reference_time, observation.reference_clock_lag = elapsed, reference, lag
        observation.reference_held, observation.reference_advance = held, advance
        observation.reference_hold_count = guideClock and guideClock.hold_count or 0
        observation.reference_hold_seconds = guideClock and guideClock.hold_seconds or 0
        observation.raw_controls = { movement = controls.movement, side_movement = controls.side_movement,
            run = controls.run, sneak = controls.sneak, jump = false, use = 0 }
        observation.applied_controls = { movement = self.controls.movement, side_movement = self.controls.sideMovement,
            run = self.controls.run, sneak = self.controls.sneak, jump = false, use = 0 }
        observation.guidance_command = applied
        observation.guidance_speed_basis = guided and 'Actor.getRunSpeed/getWalkSpeed; current_speed_read_separately' or nil
        observation.camera_yaw_after_write, observation.camera_pitch_after_write = camera.getYaw(), camera.getPitch()
        observation.actor_yaw_after_write, observation.actor_pitch_after_write = self.rotation:getYaw(), self.rotation:getPitch()
        observation.camera_postwrite_yaw_error = math.abs(sample.angleDelta(command.camera.yaw, camera.getYaw()))
        observation.camera_postwrite_pitch_error = math.abs(command.camera.pitch - camera.getPitch())
        cameraLayer.annotate(observation,command.camera)
        observation.postwrite_readback_phase = 'camera_setter_readback; actor_delta_not_yet_consumed_by_mechanics'
        postwriteYawError = math.max(postwriteYawError, observation.camera_postwrite_yaw_error)
        postwritePitchError = math.max(postwritePitchError, observation.camera_postwrite_pitch_error)
        emit('check', observation)
    end
    local markers = timeline.markers or {}
    while markerIndex <= #markers and markers[markerIndex].t <= reference do
        emit('marker', { index = markers[markerIndex].index or markerIndex, expected_time = markers[markerIndex].t })
        markerIndex = markerIndex + 1
    end
end
return {
    engineHandlers = {
        onFrame = function(dt)
            local ok, failure = pcall(frame, dt)
            if not ok then
                emit('error', { message = tostring(failure):sub(1, 512) })
                local released, releaseFailure = pcall(release)
                if not released then emit('error', { message = tostring(releaseFailure):sub(1, 512) }) end
                close('playback_error')
            end
        end,
        onKeyPress = function(key)
            if key.code == input.KEY.F11 and not key.withCtrl and not key.withShift and not key.withAlt and not key.withSuper then close('user_aborted') end
        end,
        onSave = function() return { schema = 1, playback_state = state } end,
        onLoad = function(data)
            -- A newly added script receives onInit instead. Loading after a preview
            -- started must not resume a half-replayed route.
            if data and data.playback_state and data.playback_state ~= 'waiting' then close('reloaded_playback_state') end
        end,
    },
    eventHandlers = {
        OptimizedMWRoutePolicy = function(data)
            if type(data) ~= 'table' or data.session ~= options.session then return end
            if data.event == 'policy_ready' or data.event == 'policy_stopped' then policy = data end
            if data.event == 'policy_stopped' then
                stopAcknowledged = true
                if (data.combat_during_recording or 0) > 0 then finalReason = 'combat_observed' end
                if data.failed or (data.failure_count or 0) > 0 then finalReason = 'hostile_policy_failed' end
            end
            if data.event == 'combat' then
                emit('combat', { observation = data })
                if state == 'playing' then close('combat_observed') end
                if state == 'closing' then finalReason = 'combat_observed' end
            end
        end,
    },
}
