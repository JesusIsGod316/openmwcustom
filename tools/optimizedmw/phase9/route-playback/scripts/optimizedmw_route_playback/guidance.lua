-- Optional physical XY feedback for a private preview. All outputs are ordinary
-- movement inputs. This module never moves an object or changes actor stats.
local M = { PROTOCOL = 'physical_xy_follower_v1' }
M.limits = { gain = 2, correction_norm = 0.35, input_norm = 1,
    hold_enter_xy = 32, hold_exit_xy = 16, hard_xy = 96, hard_z = 96,
    setup_xy = 6, setup_z = 48, setup_input_norm = 0.2, setup_seconds = 5,
    finish_xy = 24, finish_z = 48, stall_seconds = 2, stall_progress = 8,
    stall_input_norm = 0.25, maximum_lag = 15 }
local function finite(n) return type(n) == 'number' and n == n and n ~= math.huge and n ~= -math.huge end
local function norm(x, y) return math.sqrt(x*x + y*y) end
local function cap(x, y, maximum)
    local length = norm(x, y)
    if length > maximum then return x * maximum / length, y * maximum / length end
    return x, y
end
function M.errors(actual, expected)
    local dx, dy, dz = expected.x - actual.x, expected.y - actual.y, expected.z - actual.z
    return norm(dx, dy), math.abs(dz), { x = dx, y = dy, z = dz }
end
-- Engine yaw zero faces +Y, with positive yaw toward +X. Side movement is
-- actor-right. Projection uses the heading consumed by this frame's mechanics.
function M.worldToLocal(x, y, yaw)
    return x * math.sin(yaw) + y * math.cos(yaw), x * math.cos(yaw) - y * math.sin(yaw)
end
function M.localToWorld(movement, side, yaw)
    return movement * math.sin(yaw) + side * math.cos(yaw), movement * math.cos(yaw) - side * math.sin(yaw)
end
local function speedChecked(speed)
    assert(finite(speed) and speed > 0, 'Guided preview requires a positive actor gait speed')
    return speed
end
function M.setupCommand(actual, expected, yaw, speed)
    speedChecked(speed)
    local xy, z, delta = M.errors(actual, expected)
    local forward, side = M.worldToLocal(delta.x * M.limits.gain / speed, delta.y * M.limits.gain / speed, yaw)
    forward, side = cap(forward, side, M.limits.setup_input_norm)
    return { movement = forward, side_movement = side, input_norm = norm(forward, side),
        horizontal_error = xy, vertical_error = z, speed = speed }
end
function M.command(actual, target, raw, yaw, speed, held)
    speedChecked(speed)
    local xy, z, delta = M.errors(actual, target)
    local stationary = raw.movement == 0 and raw.side_movement == 0
    local correctionForward, correctionSide = 0, 0
    -- Source stops and the fixed camera view remain stationary. Initial physical
    -- alignment is its own bounded setup phase, before the route clock starts.
    if not stationary then
        correctionForward, correctionSide = M.worldToLocal(delta.x * M.limits.gain / speed,
            delta.y * M.limits.gain / speed, yaw)
        correctionForward, correctionSide = cap(correctionForward, correctionSide, M.limits.correction_norm)
    end
    local nominalForward, nominalSide = raw.movement, raw.side_movement
    if held then nominalForward, nominalSide = 0, 0 end
    local forward, side = cap(nominalForward + correctionForward, nominalSide + correctionSide, M.limits.input_norm)
    return { movement = forward, side_movement = side, input_norm = norm(forward, side),
        correction_movement = correctionForward, correction_side_movement = correctionSide,
        correction_norm = norm(correctionForward, correctionSide),
        nominal_movement = nominalForward, nominal_side_movement = nominalSide,
        target_horizontal_error = xy, target_vertical_error = z, speed = speed,
        stationary_source = stationary }
end
function M.newClock(duration)
    assert(finite(duration) and duration > 0 and duration <= 180, 'Invalid guided route duration')
    local clock = { reference = 0, source = 0, held = false, hold_count = 0, hold_seconds = 0 }
    function clock:advance(source)
        assert(finite(source) and source >= self.source, 'Guided source clock cannot reverse')
        local delta = source - self.source
        if self.held then self.hold_seconds = self.hold_seconds + delta
        else self.reference = math.min(duration, self.reference + delta) end
        self.source = source
        return self.reference, self.source - self.reference
    end
    function clock:updateHold(error)
        assert(finite(error) and error >= 0, 'Invalid guided horizontal error')
        local before = self.held
        if self.held then
            if error < M.limits.hold_exit_xy then self.held = false end
        elseif error > M.limits.hold_enter_xy then self.held = true; self.hold_count = self.hold_count + 1 end
        return self.held, before ~= self.held
    end
    function clock:futureDelta(dt)
        assert(finite(dt) and dt >= 0, 'Invalid guided frame delta')
        return self.held and 0 or math.min(dt, duration - self.reference)
    end
    return clock
end
function M.newStallGuard()
    local guard = { time = nil, position = nil }
    function guard:observe(time, position, inputNorm)
        if inputNorm <= M.limits.stall_input_norm then self.time, self.position = nil, nil; return false end
        if not self.time then self.time, self.position = time, { x = position.x, y = position.y }; return false end
        local progress = norm(position.x - self.position.x, position.y - self.position.y)
        if progress >= M.limits.stall_progress then
            self.time, self.position = time, { x = position.x, y = position.y }
            return false
        end
        return time - self.time >= M.limits.stall_seconds
    end
    return guard
end
function M.finalPoseAccepted(actual, expected)
    local xy, z = M.errors(actual, expected)
    return xy <= M.limits.finish_xy and z <= M.limits.finish_z, xy, z
end
return M
