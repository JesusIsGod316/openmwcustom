-- Observe AI on private calibration actors without editing AI packages or Mercy.
local core = require('openmw.core')
local self = require('openmw.self')
local AI = require('openmw.interfaces').AI
local options = require('scripts.optimizedmw_route_recorder.options')
local enabled = options.no_enemy_policy and options.no_enemy_policy.enabled == true
local previous = {}
local failed = false

local function reportError(reason)
    if failed then return end
    failed = true
    core.sendGlobalEvent('OptimizedMWRoutePolicyObserverError', {
        session = options.session, actor = self, reason = reason,
    })
end

local function inspect()
    if not enabled or failed then return end
    if not AI or not AI.getTargets then
        reportError('AI.getTargets interface unavailable')
        return
    end
    for _, kind in ipairs({ 'Combat', 'Pursue' }) do
        local ok, targets = pcall(AI.getTargets, kind)
        if not ok then
            reportError(tostring(targets))
            return
        end
        local ids = {}
        for _, actor in ipairs(targets) do ids[#ids + 1] = tostring(actor.id) end
        table.sort(ids)
        local fingerprint = table.concat(ids, '|')
        if #ids > 0 and previous[kind] ~= fingerprint then
            core.sendGlobalEvent('OptimizedMWRouteCombatDetected', {
                session = options.session,
                actor = self,
                kind = kind,
                target_count = #ids,
                first_target_reference_id = ids[1],
                observed_t_wall = core.getRealTime(),
                observed_t_sim = core.getSimulationTime(),
            })
        end
        previous[kind] = fingerprint
    end
end

return {
    engineHandlers = {
        onActive = inspect,
        onUpdate = inspect,
        onInactive = function() previous = {} end,
    },
}
