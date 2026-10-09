-- Private route-calibration policy. This is not a gameplay or Mercy replacement.
local core = require('openmw.core')
local world = require('openmw.world')
local types = require('openmw.types')
local options = require('scripts.optimizedmw_route_recorder.options')

local cfg = options.no_enemy_policy or {}
local enabled = cfg.enabled == true
local discover = cfg.discover_high_fight_creatures == true
local threshold = cfg.creature_fight_threshold or 90
assert(type(threshold) == 'number' and threshold >= 90 and threshold <= 100,
    'Creature fight threshold must be between 90 and 100')

local function makeSet(values, lower)
    local result = {}
    assert(type(values or {}) == 'table', 'Hostile reference/record list must be a table')
    assert(#(values or {}) <= 4096, 'Hostile reference/record list exceeds 4096 entries')
    for _, value in ipairs(values or {}) do
        assert(type(value) == 'string' and #value > 0 and #value <= 512, 'Invalid hostile identifier')
        result[lower and value:lower() or value] = true
    end
    return result
end

local references = makeSet(cfg.hostile_reference_ids, false)
local records = makeSet(cfg.hostile_record_ids, true)
local player
local recording = false
local recordingStart
local recordingEnd
local suppressed = {}
local pending = {}
local attemptedCount = 0
local confirmedCount = 0
local combatBefore = 0
local combatDuring = 0
local failed = false
local failureReason
local failureCount = 0
local startupUpdates = 0
local startupTime
local readySent = false
local LIMIT = 4096

local function finiteNumber(value)
    return type(value) == 'number' and value == value and value ~= math.huge and value ~= -math.huge
end

local function pendingCount()
    local count = 0
    for _ in pairs(pending) do count = count + 1 end
    return count
end

local function identity(actor)
    local cell = actor.cell
    return {
        reference_id = tostring(actor.id),
        record_id = actor.recordId,
        actor_type = types.Creature.objectIsInstance(actor) and 'Creature' or 'NPC',
        cell_id = cell and cell.id or nil,
        cell_name = cell and cell.name or nil,
        cell_exterior = cell and cell.isExterior or nil,
    }
end

local function emit(data)
    if not player or not player:isValid() then return end
    data.session = options.session
    data.t_wall = core.getRealTime()
    data.t_sim = core.getSimulationTime()
    player:sendEvent('OptimizedMWRoutePolicy', data)
end

local function snapshot()
    return {
        event = 'policy_ready',
        enabled = enabled,
        ready = readySent and not failed and pendingCount() == 0,
        failed = failed,
        failure_reason = failureReason,
        failure_count = failureCount,
        suppressed_count = confirmedCount,
        attempted_count = attemptedCount,
        pending_disable = pendingCount(),
        combat_before_start = combatBefore,
        combat_during_recording = combatDuring,
        qualified = enabled and readySent and not failed and pendingCount() == 0 and combatDuring == 0,
        explicit_reference_count = #(cfg.hostile_reference_ids or {}),
        explicit_record_count = #(cfg.hostile_record_ids or {}),
        discover_high_fight_creatures = discover,
        creature_fight_threshold = threshold,
        npc_fight_heuristic = false,
        combat_observer = 'local_ai_targets_per_update',
        preemption_guaranteed = false,
        limits = 'Fight discovery is incomplete; disable is queued and local scripts may run before it. Any combat during recording needs review before replay.',
    }
end

local function publish()
    emit(snapshot())
end

local function problem(reason, actor)
    failed = true
    failureReason = failureReason or reason:sub(1, 512)
    failureCount = failureCount + 1
    local data = actor and identity(actor) or {}
    data.event = 'policy_error'
    data.reason = reason
    emit(data)
    publish()
end

local function suppress(actor, reason, fight)
    if not enabled or not actor or not actor:isValid() or types.Player.objectIsInstance(actor) then return end
    local id = tostring(actor.id)
    if suppressed[id] or pending[id] or not actor.enabled then return end
    if attemptedCount >= LIMIT then
        problem('suppression_limit_exceeded', actor)
        return
    end
    local data = identity(actor)
    data.reason = reason
    data.fight = fight
    data.event = 'actor_suppression_requested'
    attemptedCount = attemptedCount + 1
    -- enabled assignment queues an engine action; confirmation is a later update.
    local ok, err = pcall(function() actor.enabled = false end)
    if not ok then
        problem('disable_failed: ' .. tostring(err), actor)
        return
    end
    pending[id] = { actor = actor, data = data, requested = core.getRealTime() }
    emit(data)
    if readySent then publish() end
end

local function classify(actor)
    if not enabled or not actor or not actor:isValid() or types.Player.objectIsInstance(actor) then return end
    if references[tostring(actor.id)] then
        suppress(actor, 'explicit_reference')
        return
    end
    if records[actor.recordId:lower()] then
        suppress(actor, 'explicit_record')
        return
    end
    -- This is an opt-in discovery heuristic, never an NPC/citizen classifier.
    if discover and types.Creature.objectIsInstance(actor) then
        local ok, fight = pcall(function() return types.Actor.stats.ai.fight(actor).modified end)
        if not ok or not finiteNumber(fight) then
            problem('creature_fight_discovery_unavailable', actor)
        elseif fight >= threshold then
            suppress(actor, 'high_fight_creature_discovery', fight)
        end
    end
end

local function onUpdate()
    if not player then return end
    startupUpdates = startupUpdates + 1
    local changed = false
    for id, item in pairs(pending) do
        local actor = item.actor
        if not actor:isValid() then
            pending[id] = nil
            problem('actor_invalid_before_disable_confirmation')
        elseif not actor.enabled then
            changed = true
            pending[id] = nil
            suppressed[id] = item.data
            confirmedCount = confirmedCount + 1
            item.data.event = 'actor_suppressed'
            item.data.confirmed_disabled = true
            item.data.during_recording = recording
            emit(item.data)
        elseif core.getRealTime() - item.requested > 5 then
            pending[id] = nil
            problem('disable_confirmation_timeout', actor)
        end
    end
    if not readySent and startupUpdates >= 2 and (pendingCount() == 0 or core.getRealTime() - startupTime > 5) then
        readySent = true
        if pendingCount() > 0 then failed = true end
        publish()
    elseif readySent and changed then
        publish()
    end
end

return {
    engineHandlers = {
        onPlayerAdded = function(actor)
            player = actor
            startupTime = core.getRealTime()
            startupUpdates = 0
            for _, active in ipairs(world.activeActors) do classify(active) end
        end,
        onActorActive = classify,
        onUpdate = onUpdate,
    },
    eventHandlers = {
        OptimizedMWRouteRecording = function(data)
            if data.session ~= options.session then return end
            recording = data.active == true
            local wall = finiteNumber(data.t_wall) and data.t_wall or core.getRealTime()
            if recording then
                combatDuring = 0
                recordingStart = wall
                recordingEnd = nil
            else
                recordingEnd = wall
            end
            publish()
            if not recording then
                local stopped = snapshot()
                stopped.event = 'policy_stopped'
                stopped.recording_start_wall = recordingStart
                stopped.recording_end_wall = recordingEnd
                emit(stopped)
            end
        end,
        OptimizedMWRouteCombatDetected = function(data)
            if not enabled or data.session ~= options.session then return end
            local actor = data.actor
            if not actor or not actor:isValid() or types.Player.objectIsInstance(actor) then return end
            local observed = data.observed_t_wall
            local during
            if finiteNumber(observed) then
                during = recordingStart ~= nil and observed >= recordingStart
                    and (recordingEnd == nil or observed <= recordingEnd)
            else
                -- Missing timing cannot exonerate combat around a start/stop boundary.
                during = recording or recordingStart ~= nil
            end
            if during then combatDuring = combatDuring + 1 else combatBefore = combatBefore + 1 end
            local event = identity(actor)
            event.event = 'combat'
            event.kind = data.kind
            event.during_recording = during
            event.observed_t_wall = finiteNumber(observed) and observed or nil
            event.observed_t_sim = data.observed_t_sim
            event.target_count = data.target_count
            event.first_target_reference_id = data.first_target_reference_id
            event.reason = 'unexpected_combat_or_pursuit'
            emit(event)
            suppress(actor, 'reactive_' .. tostring(data.kind))
            publish()
        end,
        OptimizedMWRoutePolicyObserverError = function(data)
            if data.session ~= options.session then return end
            local actor = data.actor
            if actor and not actor:isValid() then actor = nil end
            problem('combat_observer_failed: ' .. tostring(data.reason), actor)
        end,
    },
}
