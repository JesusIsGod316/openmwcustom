-- Run with LuaJIT 5.1: luajit policy-tests.lua <route-recorder directory>.
-- Engine mocks explicitly delay enabled changes, so admission is not mistaken for confirmation.
local base = assert(arg[1], 'route-recorder directory required')
local checks = 0
local function check(value, message)
    checks = checks + 1
    assert(value, message)
end
local function copy(value)
    if type(value) ~= 'table' then return value end
    local result = {}
    for k, v in pairs(value) do result[k] = copy(v) end
    return result
end
local function loadModule(path, mocks)
    local fn = assert(loadfile(base .. '/' .. path))
    setfenv(fn, setmetatable({ require = function(name)
        return assert(mocks[name], 'Unexpected dependency: ' .. name)
    end }, { __index = _G }))
    return fn()
end
local function fixture(config)
    local events, delayed = {}, {}
    local now = 0
    local opts = {
        session = '0123456789abcdef0123456789abcdef',
        no_enemy_policy = config or {
            enabled = true, discover_high_fight_creatures = true, creature_fight_threshold = 90,
        },
    }
    local function actor(id, kind, fight, record)
        local state = { enabled = true, valid = true }
        local a = {
            id = id, recordId = record or id, _kind = kind, _fight = fight or 0,
            cell = { id = 'Balmora', name = 'Balmora', isExterior = true },
            isValid = function() return state.valid end,
            sendEvent = function(_, name, data) events[#events + 1] = { name = name, data = copy(data) } end,
        }
        return setmetatable(a, {
            __index = function(_, key) if key == 'enabled' then return state.enabled end end,
            __newindex = function(_, key, value)
                assert(key == 'enabled', 'Unexpected actor mutation: ' .. key)
                delayed[#delayed + 1] = function() state.enabled = value end
            end,
        }), state
    end
    local player = actor('player', 'Player', 100)
    local world = { activeActors = {} }
    local core = { getRealTime = function() return now end, getSimulationTime = function() return now end }
    local types = {
        Player = { objectIsInstance = function(a) return a._kind == 'Player' end },
        Creature = { objectIsInstance = function(a) return a._kind == 'Creature' end },
        Actor = { stats = { ai = { fight = function(a) return { modified = a._fight } end } } },
    }
    local script = loadModule('scripts/optimizedmw_route/policy.lua', {
        ['openmw.core'] = core, ['openmw.world'] = world, ['openmw.types'] = types,
        ['scripts.optimizedmw_route_recorder.options'] = opts,
    })
    local function advance(seconds, apply)
        now = now + (seconds or 0.02)
        if apply ~= false then
            local queue = delayed
            delayed = {}
            for _, action in ipairs(queue) do action() end
        end
        script.engineHandlers.onUpdate()
    end
    local function last()
        for i = #events, 1, -1 do
            if events[i].data.event == 'policy_ready' then return events[i].data end
        end
    end
    return { script = script, actor = actor, player = player, world = world, events = events,
        advance = advance, last = last, options = opts }
end

do
    local f = fixture()
    local hostile = f.actor('ref-hostile', 'Creature', 90, 'Cliff_Racer')
    local citizen = f.actor('ref-citizen', 'NPC', 100, 'Town_Citizen')
    local low = f.actor('ref-low', 'Creature', 89)
    f.world.activeActors = { f.player, citizen, hostile, low }
    f.script.engineHandlers.onPlayerAdded(f.player)
    check(hostile.enabled, 'disable must remain queued before action application')
    check(citizen.enabled and low.enabled and f.player.enabled, 'ordinary NPCs, low fight creatures and player preserved')
    f.advance(0.02, false)
    check(f.last() == nil, 'readiness not announced before second startup update')
    f.advance()
    check(not hostile.enabled, 'queued hostile disable applied')
    check(f.last().ready and f.last().qualified, 'settled setup ready with no observed combat')
    check(f.last().suppressed_count == 1 and f.last().pending_disable == 0, 'one confirmed disable')
    local receipt
    for _, event in ipairs(f.events) do if event.data.event == 'actor_suppressed' then receipt = event.data end end
    check(receipt.reference_id == 'ref-hostile', 'exact initial reference retained in individual receipt')
    check(receipt.reason == 'high_fight_creature_discovery' and not receipt.during_recording, 'discovery reason and initial phase retained')
    check(f.last().preemption_guaranteed == false and f.last().npc_fight_heuristic == false, 'limitations explicit')
    f.script.engineHandlers.onActorActive(hostile)
    f.advance()
    check(f.last().attempted_count == 1, 'repeat activation does not double count')
end

do
    local f = fixture({ enabled = true, hostile_reference_ids = { 'specific-npc', 'player' },
        hostile_record_ids = { 'Mixed_CASE' }, discover_high_fight_creatures = false })
    local npc = f.actor('specific-npc', 'NPC', 0)
    local record = f.actor('other-ref', 'NPC', 0, 'mixed_case')
    local creature = f.actor('other-creature', 'Creature', 100)
    f.world.activeActors = { f.player, npc, record, creature }
    f.script.engineHandlers.onPlayerAdded(f.player)
    f.advance()
    f.advance()
    check(not npc.enabled and not record.enabled, 'explicit references and case insensitive records disabled')
    check(f.player.enabled and creature.enabled, 'player immune and heuristic can be disabled')
    check(f.last().suppressed_count == 2, 'only explicit matches counted')
end

do
    -- The known Seyda encounter was a rat, but its original Fight decision was
    -- not captured. An explicit record match must not rely on that heuristic.
    local f = fixture({ enabled = true, hostile_record_ids = { 'rat' },
        discover_high_fight_creatures = true, creature_fight_threshold = 90 })
    local citizen = f.actor('seyda-citizen', 'NPC', 100, 'town_citizen')
    local harmless = f.actor('other-low-fight-creature', 'Creature', 0, 'fixture_creature')
    f.world.activeActors = { f.player, citizen, harmless }
    f.script.engineHandlers.onPlayerAdded(f.player)
    f.advance()
    f.advance()
    check(f.last().qualified, 'initial ordinary actors settle without suppression')
    f.script.eventHandlers.OptimizedMWRouteRecording({ session = f.options.session, active = true, t_wall = 0.04 })
    local rat = f.actor('new-cell-rat', 'Creature', 0, 'RaT')
    f.script.engineHandlers.onActorActive(rat)
    check(rat.enabled and f.last().pending_disable == 1, 'new-cell explicit rat removal is requested but still queued')
    check(f.last().attempted_count == 1 and f.last().suppressed_count == 0,
        'rat admission request is not counted as confirmed removal')
    check(not f.last().qualified and not f.last().ready,
        'a pending new-cell disable temporarily prevents policy qualification')
    local request
    for _, event in ipairs(f.events) do
        if event.data.event == 'actor_suppression_requested' then request = event.data end
    end
    check(request and request.reference_id == 'new-cell-rat' and request.record_id == 'RaT'
        and request.reason == 'explicit_record', 'case insensitive low-Fight rat match retains explicit reason and identity')
    f.advance(0.02, false)
    check(rat.enabled and f.last().suppressed_count == 0, 'another update cannot fabricate confirmation before engine action')
    f.advance()
    check(not rat.enabled and f.last().pending_disable == 0 and f.last().suppressed_count == 1,
        'rat removal confirms only after delayed engine action applies')
    check(f.last().qualified and f.last().combat_during_recording == 0,
        'explicit rat admission can settle without observed combat')
    check(citizen.enabled and harmless.enabled and f.player.enabled,
        'targeted rat policy preserves townspeople, unrelated low-Fight creatures and player')
    local receipt
    for _, event in ipairs(f.events) do
        if event.data.event == 'actor_suppressed' then receipt = event.data end
    end
    check(receipt and receipt.during_recording and receipt.confirmed_disabled
        and receipt.t_wall > request.t_wall, 'receipt separates in-record confirmation from prior request time')
    f.script.engineHandlers.onActorActive(rat)
    f.advance()
    check(f.last().attempted_count == 1, 'confirmed rat reactivation does not duplicate suppression receipts')
end

do
    local f = fixture({ enabled = true, hostile_record_ids = { 'rat' },
        discover_high_fight_creatures = false })
    f.script.engineHandlers.onPlayerAdded(f.player)
    f.advance()
    f.advance()
    f.script.eventHandlers.OptimizedMWRouteRecording({ session = f.options.session, active = true, t_wall = 0.04 })
    local rat = f.actor('rat-combat-before-confirmation', 'Creature', 0, 'rat')
    f.script.engineHandlers.onActorActive(rat)
    f.advance(0.02, false)
    f.script.eventHandlers.OptimizedMWRouteCombatDetected({ session = f.options.session, actor = rat,
        kind = 'Combat', observed_t_wall = 0.05, first_target_reference_id = 'player', target_count = 1 })
    check(rat.enabled and f.last().combat_during_recording == 1 and f.last().pending_disable == 1,
        'combat between explicit rat request and confirmation remains visible and disqualifying')
    check(f.last().attempted_count == 1, 'reactive combat receipt does not duplicate existing rat disable request')
    f.advance()
    check(not rat.enabled and f.last().suppressed_count == 1 and f.last().pending_disable == 0,
        'combat-contaminated rat removal still drains and confirms')
    check(not f.last().qualified and f.last().combat_during_recording == 1,
        'later confirmed rat removal cannot erase combat contamination')
    check(f.last().preemption_guaranteed == false, 'explicit rat policy does not claim guaranteed combat preemption')
end

do
    local f = fixture({ enabled = false, hostile_record_ids = { 'rat' },
        discover_high_fight_creatures = true, creature_fight_threshold = 90 })
    local initialRat = f.actor('initial-control-rat', 'Creature', 100, 'rat')
    f.world.activeActors = { initialRat }
    f.script.engineHandlers.onPlayerAdded(f.player)
    f.advance()
    f.advance()
    f.script.eventHandlers.OptimizedMWRouteRecording({ session = f.options.session, active = true, t_wall = 0.04 })
    local newRat = f.actor('new-cell-control-rat', 'Creature', 0, 'rat')
    f.script.engineHandlers.onActorActive(newRat)
    f.advance()
    check(initialRat.enabled and newRat.enabled, 'disabled policy leaves both initial and new-cell rats intact')
    check(f.last().attempted_count == 0 and f.last().suppressed_count == 0 and f.last().pending_disable == 0,
        'disabled explicit rat control performs no queued actor mutations')
    check(not f.last().qualified, 'unsuppressed control cannot falsely claim no-enemy qualification')
end

do
    local f = fixture()
    local citizen = f.actor('citizen-entering-combat', 'NPC', 0)
    f.script.engineHandlers.onPlayerAdded(f.player)
    f.advance()
    f.advance()
    f.script.eventHandlers.OptimizedMWRouteRecording({ session = 'wrong', active = true })
    f.script.eventHandlers.OptimizedMWRouteCombatDetected({ session = f.options.session, actor = citizen,
        kind = 'Combat', targets = { 'player' } })
    f.advance()
    check(f.last().combat_before_start == 1 and f.last().combat_during_recording == 0, 'foreign recording session ignored')
    check(not citizen.enabled, 'unexpected combat actor reactively suppressed')
    f.script.eventHandlers.OptimizedMWRouteRecording({ session = f.options.session, active = true })
    local guard = f.actor('guard-pursuing-player', 'NPC', 0)
    f.script.eventHandlers.OptimizedMWRouteCombatDetected({ session = f.options.session, actor = guard,
        kind = 'Pursue', targets = { 'player' } })
    f.advance()
    check(f.last().combat_during_recording == 1 and not f.last().qualified, 'recording combat invalidates qualification')
    check(not guard.enabled and f.last().suppressed_count == 2, 'pursuing actor removed in private calibration')
end

do
    local f = fixture()
    local hostile = f.actor('timeout-hostile', 'Creature', 100)
    f.world.activeActors = { hostile }
    f.script.engineHandlers.onPlayerAdded(f.player)
    f.advance(0.02, false)
    f.advance(6, false)
    check(f.last().failed and not f.last().qualified, 'unconfirmed disable times out instead of silently qualifying')
    check(f.last().suppressed_count == 0, 'queued attempt is not counted as confirmed suppression')
end

do
    local f = fixture({ enabled = false, discover_high_fight_creatures = true })
    local hostile = f.actor('control-hostile', 'Creature', 100)
    f.world.activeActors = { hostile }
    f.script.engineHandlers.onPlayerAdded(f.player)
    f.advance()
    f.advance()
    check(hostile.enabled and f.last().attempted_count == 0, 'disabled policy retains control actors')
    check(not f.last().qualified, 'disabled no-enemy policy cannot claim qualification')
end

do
    local f = fixture()
    local broken = f.actor('unknown-fight', 'Creature', math.huge)
    f.world.activeActors = { broken }
    f.script.engineHandlers.onPlayerAdded(f.player)
    f.advance()
    f.advance()
    check(f.last().failed and not f.last().qualified and f.last().failure_reason == 'creature_fight_discovery_unavailable',
        'unavailable fight discovery fails coverage rather than silently skipping actor')
end

do
    local events = {}
    local targets = { Combat = {}, Pursue = {} }
    local opts = { session = '0123456789abcdef0123456789abcdef', no_enemy_policy = { enabled = true } }
    local actor = { id = 'local-hostile' }
    local script = loadModule('scripts/optimizedmw_route/policy_actor.lua', {
        ['openmw.core'] = {
            sendGlobalEvent = function(name, data) events[#events + 1] = { name = name, data = data } end,
            getRealTime = function() return 10 end, getSimulationTime = function() return 8 end,
        },
        ['openmw.self'] = actor,
        ['openmw.interfaces'] = { AI = { getTargets = function(kind) return targets[kind] end } },
        ['scripts.optimizedmw_route_recorder.options'] = opts,
    })
    script.engineHandlers.onActive()
    check(#events == 0, 'no event for noncombat actors')
    targets.Combat = { { id = 'player' } }
    script.engineHandlers.onUpdate()
    check(#events == 1 and events[1].data.kind == 'Combat', 'combat transition emitted')
    check(events[1].data.observed_t_wall == 10 and events[1].data.observed_t_sim == 8, 'observation clocks stamped at actor rather than delayed global delivery')
    script.engineHandlers.onUpdate()
    check(#events == 1, 'unchanged combat targets do not spam events')
    targets.Pursue = { { id = 'player' } }
    script.engineHandlers.onUpdate()
    check(#events == 2 and events[2].data.kind == 'Pursue', 'pursuit transition emitted')
    targets.Combat = {}
    script.engineHandlers.onUpdate()
    targets.Combat = { { id = 'player' } }
    script.engineHandlers.onUpdate()
    check(#events == 3, 'new combat after peaceful state observed')
end

do
    local f = fixture()
    f.script.engineHandlers.onPlayerAdded(f.player)
    f.advance()
    f.advance()
    f.script.eventHandlers.OptimizedMWRouteRecording({ session = f.options.session, active = true, t_wall = 10 })
    local before = f.actor('observed-before-start', 'NPC', 0)
    f.script.eventHandlers.OptimizedMWRouteCombatDetected({ session = f.options.session, actor = before,
        kind = 'Combat', observed_t_wall = 9 })
    check(f.last().combat_during_recording == 0, 'delayed prestart observation not misclassified as recording combat')
    f.script.eventHandlers.OptimizedMWRouteRecording({ session = f.options.session, active = false, t_wall = 20 })
    local stopped
    for _, e in ipairs(f.events) do if e.data.event == 'policy_stopped' then stopped = e.data end end
    check(stopped and stopped.recording_start_wall == 10 and stopped.recording_end_wall == 20, 'stop acknowledgement includes observed interval and pending status')
    local during = f.actor('observed-before-stop-delivered-late', 'NPC', 0)
    f.script.eventHandlers.OptimizedMWRouteCombatDetected({ session = f.options.session, actor = during,
        kind = 'Pursue', observed_t_wall = 19 })
    check(f.last().combat_during_recording == 1, 'late delivery after stop still invalidates in-record combat')
    local after = f.actor('observed-after-stop', 'NPC', 0)
    f.script.eventHandlers.OptimizedMWRouteCombatDetected({ session = f.options.session, actor = after,
        kind = 'Combat', observed_t_wall = 21 })
    check(f.last().combat_during_recording == 1, 'postrecord observation excluded from recorded combat count')
end

do
    local errors = 0
    local script = loadModule('scripts/optimizedmw_route/policy_actor.lua', {
        ['openmw.core'] = { sendGlobalEvent = function(name)
            if name == 'OptimizedMWRoutePolicyObserverError' then errors = errors + 1 end
        end },
        ['openmw.self'] = {}, ['openmw.interfaces'] = {},
        ['scripts.optimizedmw_route_recorder.options'] = { session = 'a', no_enemy_policy = { enabled = true } },
    })
    script.engineHandlers.onActive()
    script.engineHandlers.onUpdate()
    check(errors == 1, 'missing combat observer reports one failure instead of false coverage')
end

print('PASS: route hostile policy ' .. checks .. ' checks; queued actions, NPC preservation, combat invalidation, observer coverage')
