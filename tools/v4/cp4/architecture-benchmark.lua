-- Private, temporary benchmark content. Never installed into normal data paths.
local core = require('openmw.core')
local start, previous, complete = nil, nil, false
local frames = {}
return { engineHandlers = { onUpdate = function()
    if complete then return end
    local now = core.getRealTime()
    if not start then start = now; print('ARCHITECTURE_BENCHMARK_START') end
    local elapsed = now - start
    -- getRealFrameDuration is the engine simulation step (clamped to 200 ms),
    -- not an uncapped wall-clock frame interval. Do not hide longer stalls.
    if previous and previous - start >= 15 then frames[#frames + 1] = (now - previous) * 1000 end
    previous = now
    if elapsed >= 45 then
        complete = true
        table.sort(frames)
        local sum = 0
        for _, value in ipairs(frames) do sum = sum + value end
        print(string.format('ARCHITECTURE_BENCHMARK_RESULT {"clock":"steady_wall","frames":%d,"mean_ms":%.6f,"median_ms":%.6f,"p95_ms":%.6f,"sample_seconds":%.6f,"elapsed_seconds":%.3f}',
            #frames, sum/#frames, frames[math.ceil(#frames/2)], frames[math.ceil(#frames*.95)], sum/1000, elapsed))
        core.quit()
    end
end } }
