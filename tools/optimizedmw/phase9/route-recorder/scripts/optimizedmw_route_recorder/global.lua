-- Read-only engine activation observation. Multiple GLOBAL onActivate handlers
-- execute in direct content order; this does not replace the standard action.
local core = require('openmw.core')
local types = require('openmw.types')
local options = require('scripts.optimizedmw_route_recorder.options')
local player
local installed = false

local function cellSnapshot(cell)
    if not cell then return nil end
    return { id = cell.id, name = cell.name, exterior = cell.isExterior,
        grid_x = cell.isExterior and cell.gridX or nil, grid_y = cell.isExterior and cell.gridY or nil }
end
local function vector(v) return { x = v.x, y = v.y, z = v.z } end
local function observeDoor(door, actor)
    local doorType = types.Door.objectIsInstance(door) and types.Door or types.ESM4Door
    local teleport = doorType.isTeleport(door)
    local data = {
        id = tostring(door.id), record_id = door.recordId, position = vector(door.position),
        cell = cellSnapshot(door.cell), teleport = teleport,
        source_wall = core.getRealTime(), source_simulation = core.getSimulationTime(),
        observation = 'engine_onActivate_actual_door_target',
    }
    if teleport then
        data.destination_cell = cellSnapshot(doorType.destCell(door))
        data.destination_position = vector(doorType.destPosition(door))
        local z, y, x = doorType.destRotation(door):getAnglesZYX()
        data.destination_rotation = { z = z, y = y, x = x }
    end
    actor:sendEvent('OptimizedMWRouteDoor', data)
end

local function observeActivation(object, actor)
    if not options.enabled or actor.type ~= types.Player then return end
    local now, simulation = core.getRealTime(), core.getSimulationTime()
    local ok, data = pcall(function()
        if not object:isValid() then error('Target became invalid before the read-only observer') end
        local z, y, x = object.rotation:getAnglesZYX()
        return {
            observed = true, target = { id = tostring(object.id), record_id = object.recordId,
                position = vector(object.position), rotation = { z = z, y = y, x = x }, cell = cellSnapshot(object.cell) },
            source_wall = now, source_simulation = simulation, action_success = 'not_observed',
            source = 'engine_onActivate_actual_target',
            door = types.Door.objectIsInstance(object) or (types.ESM4Door and types.ESM4Door.objectIsInstance(object)) or false,
        }
    end)
    if not ok then
        data = { observed = false, source = 'engine_onActivate_actual_target',
            source_wall = now, source_simulation = simulation, error = tostring(data):sub(1, 512) }
    end
    actor:sendEvent('OptimizedMWRouteActivation', data)
    if ok and data.door then
        local doorOk, doorError = pcall(observeDoor, object, actor)
        if not doorOk then
            actor:sendEvent('OptimizedMWRouteObserver', { ready = false,
                scope = 'engine_onActivate_actual_player_targets', reason = 'door_metadata_unresolved',
                target_id = data.target.id, error = tostring(doorError):sub(1, 512) })
        end
    end
    -- No action/control/actor changes and no return value that consumes activation.
end

local function onUpdate()
    if not options.enabled or installed or not player then return end
    installed = true
    player:sendEvent('OptimizedMWRouteObserver', {
        ready = true, scope = 'engine_onActivate_actual_player_targets',
        limitations = 'Identity is the actual activation event target; observation is after earlier handlers and does not prove their action succeeded. Player rendering rays are separate visual candidates.',
    })
end

return { engineHandlers = {
    onPlayerAdded = function(p) player = p end,
    onActivate = observeActivation,
    onUpdate = onUpdate,
    onLoad = function() installed = false end,
} }
