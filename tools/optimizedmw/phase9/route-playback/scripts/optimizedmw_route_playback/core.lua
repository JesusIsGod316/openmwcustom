-- Pure route sampling. Input replay uses the engine's physics; no position writes.
local M = {}
local function finite(n) return type(n) == 'number' and n == n and n ~= math.huge and n ~= -math.huge end
M.finite = finite
function M.angleDelta(target, actual) return (target - actual + math.pi) % (2 * math.pi) - math.pi end
local function vector(v)
    return type(v) == 'table' and finite(v.x) and finite(v.y) and finite(v.z)
        and math.abs(v.x) <= 1e8 and math.abs(v.y) <= 1e8 and math.abs(v.z) <= 1e8
end
local function exteriorCell(s)
    assert(type(s.cell_id) == 'string' and s.exterior == true, 'Invalid exterior cell identity')
    local x, y = s.cell_id:match('^Esm3ExteriorCell:(%-?%d+):(%-?%d+)$')
    x, y = tonumber(x), tonumber(y)
    assert(finite(x) and finite(y) and x >= -2147483648 and x <= 2147483647
        and y >= -2147483648 and y <= 2147483647
        and s.cell_id == string.format('Esm3ExteriorCell:%d:%d', x, y), 'Noncanonical exterior cell identity')
    if s.grid_x ~= nil or s.grid_y ~= nil then
        assert(finite(s.grid_x) and finite(s.grid_y) and s.grid_x == x and s.grid_y == y,
            'Exterior cell grid disagrees with identity')
    end
    assert(type(s.cell_name) == 'string' and #s.cell_name <= 512 and not s.cell_name:find('%c'),
        'Invalid exterior cell display name')
end
local function nativeCohort(cohort)
    assert(type(cohort) == 'table' and cohort.model == 'native_input_displacement_with_visual_animations'
        and cohort.private_override == true, 'Invalid native movement cohort')
    for key in pairs(cohort) do assert(key == 'model' or key == 'private_override', 'Unknown movement cohort field') end
end
function M.distance(a, b)
    local x, y, z = a.x - b.x, a.y - b.y, a.z - b.z
    return math.sqrt(x*x + y*y + z*z)
end
local function validateRows(rows, duration, check)
    assert(type(rows) == 'table' and #rows > 0 and #rows <= 60000, 'Invalid route rows')
    local previous = -math.huge
    for _, row in ipairs(rows) do
        assert(type(row) == 'table' and finite(row.t) and row.t >= 0 and row.t <= duration and row.t >= previous,
            'Route time is invalid or unordered')
        check(row)
        previous = row.t
    end
end
function M.validate(timeline)
    assert(type(timeline) == 'table' and timeline.schema == 1, 'Invalid playback timeline schema')
    assert(timeline.route_name == 'balmora' or timeline.route_name == 'existing', 'Unsupported recorded route identity')
    if timeline.route_name == 'existing' or timeline.movement_cohort ~= nil then nativeCohort(timeline.movement_cohort) end
    if timeline.movement_cohort_status ~= nil then
        assert(timeline.movement_cohort_status == (timeline.movement_cohort and 'recorded_native' or 'legacy_unrecorded'),
            'Movement cohort status disagrees with metadata')
    end
    local duration = timeline.duration
    assert(finite(duration) and duration > 0 and duration <= 180, 'Playback duration out of bounds')
    validateRows(timeline.controls, duration, function(c)
        assert(finite(c.movement) and math.abs(c.movement) <= 1 and finite(c.side_movement) and math.abs(c.side_movement) <= 1,
            'Invalid movement input')
        assert(type(c.run) == 'boolean' and type(c.sneak) == 'boolean', 'Invalid gait input')
        -- The accepted acquisition has no jumps, attacks or activations. Supporting
        -- trigger replay requires its own consumption/timing validation.
        assert(c.jump == false and c.use == 0, 'Preview does not support jump or combat triggers')
    end)
    validateRows(timeline.samples, duration, function(s)
        assert(vector(s.position), 'Invalid exterior pose sample')
        exteriorCell(s)
        assert(type(s.camera) == 'table' and finite(s.camera.yaw) and finite(s.camera.pitch)
            and finite(s.camera.fov) and s.camera.fov > 0 and s.camera.fov < math.pi and s.camera.mode == 1,
            'Preview requires finite first-person camera samples')
        assert(math.abs(s.camera.yaw) <= 2 * math.pi and math.abs(s.camera.pitch) <= math.pi/2,
            'Preview camera angles out of bounds')
        for _, key in ipairs({'extra_yaw','extra_pitch','roll'}) do
            assert(s.camera[key] == nil or s.camera[key] == 0, 'Preview does not support additional camera layers')
        end
    end)
    assert(timeline.controls[1].t == 0 and timeline.samples[1].t == 0, 'Playback requires a time-zero input and pose')
    local previous = -math.huge
    for _, marker in ipairs(timeline.markers or {}) do
        assert(finite(marker.t) and marker.t >= 0 and marker.t <= duration and marker.t >= previous,
            'Invalid marker time')
        previous = marker.t
    end
    return timeline
end
local function poseAt(timeline, poseIndex, t)
    local a, b = timeline.samples[poseIndex], timeline.samples[math.min(poseIndex + 1, #timeline.samples)]
    local alpha = b.t > a.t and math.max(0, math.min(1, (t - a.t) / (b.t - a.t))) or 0
    local function mix(x, y) return x + (y - x) * alpha end
    return {
        position = { x = mix(a.position.x, b.position.x), y = mix(a.position.y, b.position.y), z = mix(a.position.z, b.position.z) },
        cell_id = a.cell_id, cell_name = a.cell_name, exterior = a.exterior, grid_x = a.grid_x, grid_y = a.grid_y,
        camera = { yaw = a.camera.yaw + M.angleDelta(b.camera.yaw, a.camera.yaw) * alpha,
            pitch = mix(a.camera.pitch, b.camera.pitch), fov = mix(a.camera.fov, b.camera.fov), mode = a.camera.mode },
        source_index = poseIndex, source_interval = b.t - a.t,
    }
end
function M.newSampler(timeline)
    M.validate(timeline)
    local inputIndex, poseIndex, lastTime = 1, 1, -math.huge
    return function(t)
        assert(finite(t) and t >= 0 and t >= lastTime, 'Playback time cannot reverse')
        lastTime = t
        while inputIndex < #timeline.controls and timeline.controls[inputIndex + 1].t <= t do inputIndex = inputIndex + 1 end
        while poseIndex < #timeline.samples and timeline.samples[poseIndex + 1].t <= t do poseIndex = poseIndex + 1 end
        return timeline.controls[inputIndex], poseAt(timeline, poseIndex, t), inputIndex
    end
end
function M.newCommandSampler(timeline)
    M.validate(timeline)
    -- A separate pose-only sampler cannot advance the current movement-input
    -- cursor. Stateless binary search also tolerates varying future horizons.
    return function(t, dt)
        assert(finite(t) and t >= 0 and finite(dt) and dt >= 0, 'Invalid command frame time')
        local commandTime = math.min(timeline.duration, t + dt)
        local low, high = 1, #timeline.samples
        while low < high do
            local middle = math.floor((low + high + 1) / 2)
            if timeline.samples[middle].t <= commandTime then low = middle else high = middle - 1 end
        end
        return poseAt(timeline, low, commandTime), commandTime
    end
end
return M
