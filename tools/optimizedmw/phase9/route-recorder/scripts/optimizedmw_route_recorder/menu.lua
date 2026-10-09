-- A private starting-world checkpoint. saveGame executes in synchronized onFrame,
-- never in a worker callback. No original save slot is supplied or overwritten.
local menu = require('openmw.menu')
local storage = require('openmw.storage')
local options = require('scripts.optimizedmw_route_recorder.options')
local bridge = storage.playerSection('OptimizedMWRouteRecorder:' .. options.session)
bridge:setLifeTime(storage.LIFE_TIME.GameSession)
local handled

local function saveCheckpoint(request)
    if menu.getState() ~= menu.STATE.Running then error('No running game for route checkpoint') end
    local directory = menu.getCurrentSaveDir()
    if not directory then error('Load the copied benchmark save before recording a route') end
    local before = menu.getSaves(directory)
    local description = 'OptimizedMW route ' .. options.route_name .. ' ' .. options.session
    -- Omitting slotName creates a new slot, even if an older description matches.
    menu.saveGame(description)
    local after = menu.getSaves(directory)
    local filename, info
    for name, row in pairs(after) do
        if not before[name] and row.description == description then
            if filename then error('Ambiguous new route checkpoint slots') end
            filename, info = name, row
        end
    end
    if not filename then error('New route checkpoint was not visible after saveGame') end
    return { directory = directory, filename = filename, description = description,
        creation_time = info.creationTime, request = request, source = 'private_menu_new_save_slot' }
end

return { engineHandlers = { onFrame = function()
    if not options.enabled then return end
    local request = bridge:get('checkpoint_request')
    if not request or request == handled then return end
    handled = request
    local ok, result = pcall(saveCheckpoint, request)
    if ok then bridge:set('checkpoint_ack', { request = request, ok = true, save = result })
    else bridge:set('checkpoint_ack', { request = request, ok = false, error = tostring(result):sub(1, 512) }) end
end } }
