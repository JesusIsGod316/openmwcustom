-- Pure Lua, with no engine imports, file I/O, control writes, or third-party dependency.
local M = {}
local PREFIX = 'OMWROUTE|'
M.PREFIX = PREFIX

function M.finite(n)
    return type(n) == 'number' and n == n and n ~= math.huge and n ~= -math.huge
end

local function quote(s)
    return '"' .. s:gsub('[%z\1-\31\\"]', function(c)
        if c == '"' then return '\\"' end
        if c == '\\' then return '\\\\' end
        return string.format('\\u%04x', string.byte(c))
    end) .. '"'
end

local function encode(value, seen, depth)
    if depth > 12 then error('route JSON depth exceeded') end
    local kind = type(value)
    if kind == 'nil' then return 'null' end
    if kind == 'boolean' then return value and 'true' or 'false' end
    if kind == 'string' then
        if #value > 2048 then error('route JSON string exceeded 2048 bytes') end
        return quote(value)
    end
    if kind == 'number' then
        if not M.finite(value) then error('route JSON contains a nonfinite number') end
        return string.format('%.17g', value)
    end
    if kind ~= 'table' then error('route JSON unsupported value: ' .. kind) end
    if seen[value] then error('route JSON cycle') end
    seen[value] = true
    local keys, numericCount, largest = {}, 0, 0
    for key in pairs(value) do
        if type(key) == 'number' and key >= 1 and key % 1 == 0 then
            numericCount = numericCount + 1
            largest = math.max(largest, key)
        elseif type(key) ~= 'string' then error('route JSON invalid table key') end
        keys[#keys + 1] = key
    end
    if numericCount > 0 then
        if numericCount ~= #keys or largest ~= numericCount then error('route JSON array is sparse or mixed') end
        local array = {}
        for i = 1, numericCount do array[i] = encode(value[i], seen, depth + 1) end
        seen[value] = nil
        return '[' .. table.concat(array, ',') .. ']'
    end
    table.sort(keys)
    local out = {}
    for i, key in ipairs(keys) do
        out[i] = quote(key) .. ':' .. encode(value[key], seen, depth + 1)
    end
    seen[value] = nil
    return '{' .. table.concat(out, ',') .. '}'
end

function M.json(value) return encode(value, {}, 0) end

function M.validateOptions(options)
    if type(options) ~= 'table' or options.schema ~= 1 then error('route options schema must be 1') end
    if type(options.session) ~= 'string' or #options.session ~= 32 or not options.session:match('^[0-9a-f]+$') then
        error('route session must be 32 lowercase hex digits')
    end
    if type(options.route_name) ~= 'string' or #options.route_name < 1 or #options.route_name > 96
        or options.route_name:find('[%z\1-\31]') then error('route name is invalid') end
    local ranges = {
        sample_hz = { 1, 20, 20 }, max_seconds = { 1, 600, 600 },
        max_records = { 8, 60000, 60000 }, max_bytes = { 8192, 16777216, 16777216 },
    }
    for name, limits in pairs(ranges) do
        local v = options[name] or limits[3]
        if not M.finite(v) or v < limits[1] or v > limits[2] or v % 1 ~= 0 then
            error('route option out of bounds: ' .. name)
        end
        options[name] = v
    end
    local smoke = options.smoke_duration_seconds or 20
    if not M.finite(smoke) or smoke < 1 or smoke > 60 then error('smoke duration must be between 1 and 60 seconds') end
    options.smoke_duration_seconds = smoke
    return options
end

-- No per-frame print during acquisition. Ready/start are immediate; remaining lines
-- are bounded in memory, then emitted only after the recording interval is closed.
function M.newRecorder(options, emit)
    options = M.validateOptions(options)
    local r = {
        options = options, seq = 0, state = 'waiting', buffered = {}, bytes = 0,
        flags = { complete = false }, counts = {}, flush_index = 1,
    }
    local function make(event, payload, time)
        if not M.finite(time) or time < 0 then error('route wall clock is invalid') end
        local row = {}
        for key, value in pairs(payload or {}) do row[key] = value end
        row.schema, row.session, row.event, row.seq, row.t = 1, options.session, event, r.seq + 1, time
        local line = PREFIX .. M.json(row)
        if #line > 8192 then error('route record exceeded 8192 bytes') end
        return line
    end
    function r:push(event, payload, time, immediate)
        if self.state == 'exporting' or self.state == 'finished' then return false end
        local line = make(event, payload, time)
        -- Leave both a record and 4096 bytes for the terminal footer.
        if self.seq + 1 >= options.max_records or self.bytes + #line + 1 > options.max_bytes - 4096 then
            self.flags.budget_exceeded = true
            return false
        end
        self.seq = self.seq + 1
        self.bytes = self.bytes + #line + 1
        self.counts[event] = (self.counts[event] or 0) + 1
        if immediate then emit(line) else self.buffered[#self.buffered + 1] = line end
        return true
    end
    function r:ready(payload, time)
        if self.state ~= 'waiting' then return false end
        self:push('ready', payload, time, true)
        self.state = 'ready'
        return true
    end
    function r:start(payload, time)
        if self.state ~= 'ready' then return false end
        self:push('start', payload, time, true)
        self.state = 'recording'
        return true
    end
    function r:finish(reason, time, payload)
        if self.state == 'exporting' or self.state == 'finished' then return false end
        self.flags.complete = reason == 'user_finished' or reason == 'smoke_finished'
        if self.flags.budget_exceeded or self.flags.nonfinite_data or self.flags.reloaded or self.flags.combat_invalid
            or self.flags.checkpoint_failed or self.flags.activation_target_unresolved
            or self.state ~= 'recording' then self.flags.complete = false end
        local footer = payload or {}
        footer.reason = reason
        footer.flags = self.flags
        footer.expected_records = self.seq + 1
        footer.counts = self.counts
        footer.capture_bytes_before_footer = self.bytes
        local line = make('finish', footer, time)
        self.seq = self.seq + 1
        self.bytes = self.bytes + #line + 1
        self.buffered[#self.buffered + 1] = line
        self.state = 'exporting'
        return true
    end
    function r:flush(maxLines)
        if self.state ~= 'exporting' then return false end
        local endIndex = math.min(#self.buffered, self.flush_index + (maxLines or 32) - 1)
        for i = self.flush_index, endIndex do
            emit(self.buffered[i])
            self.buffered[i] = false
        end
        self.flush_index = endIndex + 1
        if self.flush_index > #self.buffered then
            self.state = 'finished'
            self.buffered = {}
            return true
        end
        return false
    end
    return r
end

return M
