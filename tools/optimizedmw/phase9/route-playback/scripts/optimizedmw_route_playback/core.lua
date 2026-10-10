-- Pure route sampling. Input replay uses the engine's physics; no position writes.
local M = {}
local function finite(n) return type(n) == 'number' and n == n and n ~= math.huge and n ~= -math.huge end
M.finite = finite
function M.angleDelta(target, actual) return (target - actual + math.pi) % (2 * math.pi) - math.pi end
local function vector(v)
    return type(v) == 'table' and finite(v.x) and finite(v.y) and finite(v.z)
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
    assert(timeline.route_name == 'balmora', 'This preview supports the recorded Balmora route only')
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
        assert(vector(s.position) and type(s.cell_id) == 'string' and s.exterior == true, 'Invalid exterior pose sample')
        assert(type(s.camera) == 'table' and finite(s.camera.yaw) and finite(s.camera.pitch)
            and finite(s.camera.fov) and s.camera.fov > 0 and s.camera.fov < math.pi and s.camera.mode == 1,
            'Preview requires finite first-person camera samples')
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
function M.newSampler(timeline)
    M.validate(timeline)
    local inputIndex, poseIndex, lastTime = 1, 1, -math.huge
    return function(t)
        assert(finite(t) and t >= 0 and t >= lastTime, 'Playback time cannot reverse')
        lastTime = t
        while inputIndex < #timeline.controls and timeline.controls[inputIndex + 1].t <= t do inputIndex = inputIndex + 1 end
        while poseIndex < #timeline.samples and timeline.samples[poseIndex + 1].t <= t do poseIndex = poseIndex + 1 end
        local a, b = timeline.samples[poseIndex], timeline.samples[math.min(poseIndex + 1, #timeline.samples)]
        local alpha = b.t > a.t and math.max(0, math.min(1, (t - a.t) / (b.t - a.t))) or 0
        local function mix(x, y) return x + (y - x) * alpha end
        return timeline.controls[inputIndex], {
            position = { x = mix(a.position.x, b.position.x), y = mix(a.position.y, b.position.y), z = mix(a.position.z, b.position.z) },
            cell_id = a.cell_id, cell_name = a.cell_name, exterior = a.exterior,
            camera = { yaw = a.camera.yaw + M.angleDelta(b.camera.yaw, a.camera.yaw) * alpha,
                pitch = mix(a.camera.pitch, b.camera.pitch), fov = mix(a.camera.fov, b.camera.fov), mode = a.camera.mode },
            source_index = poseIndex, source_interval = b.t - a.t,
        }, inputIndex
    end
end
return M
