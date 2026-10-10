local root = assert(arg[1], 'package directory required')
local pure = dofile(root .. '/scripts/optimizedmw_route_playback/core.lua')
local checks = 0
local function check(condition, description)
    checks = checks + 1
    assert(condition, description)
end
local function near(a, b) return math.abs(a-b) < 0.00001 end
local function row(t, x, yaw, cell)
    return { t=t, position={x=x,y=0,z=0}, cell_id=cell or 'Balmora1', cell_name='Balmora', exterior=true,
        camera={yaw=yaw,pitch=0.1,fov=1.5,mode=1} }
end
local function input(t, movement)
    return {t=t,movement=movement,side_movement=0,run=true,sneak=false,jump=false,use=0}
end
local function route()
    return {schema=1,route_name='balmora',duration=2,controls={input(0,0),input(1,1),input(1.8,0)},
        samples={row(0,0,math.pi-0.1),row(1,10,-math.pi+0.1),row(2,20,-math.pi+0.3,'Balmora2')},
        markers={{t=1,index=1}}}
end
local lookup = pure.newSampler(route())
local c, p, index = lookup(0.5)
check(c.movement == 0 and index == 1, 'previous control remains until its time')
check(near(p.position.x,5), 'expected pose interpolates by simulation time')
check(near(math.abs(p.camera.yaw),math.pi), 'camera interpolation uses shortest wrap')
check(p.cell_id == 'Balmora1', 'cell does not change before crossing time')
c,p,index = lookup(1)
check(c.movement == 1 and index == 2, 'boundary consumes new input')
c,p = lookup(1.9)
check(c.movement == 0 and near(p.position.x,19), 'skipped frames catch up to current controls')
c,p = lookup(2)
check(p.cell_id == 'Balmora2' and near(p.position.x,20), 'final pose endpoint')
check(not pcall(lookup,1.5), 'reverse time rejected')
check(near(pure.angleDelta(-math.pi+0.1, math.pi-0.1),0.2), 'shortest positive heading')
check(near(pure.angleDelta(math.pi-0.1,-math.pi+0.1),-0.2), 'shortest negative heading')
check(pure.distance({x=0,y=0,z=0},{x=3,y=4,z=0}) == 5, 'actual path deviation measured')
local r = route(); r.controls[2].t=-1
check(not pcall(pure.validate,r), 'unordered/negative controls rejected')
r=route(); r.samples[1].t=0.1
check(not pcall(pure.validate,r), 'missing initial pose rejected')
r=route(); r.controls[1].jump=true
check(not pcall(pure.validate,r), 'unsupported trigger behavior rejected')
r=route(); r.samples[1].camera.mode=2
check(not pcall(pure.validate,r), 'unsupported camera mode rejected')
r=route(); r.samples[1].camera.yaw=0/0
check(not pcall(pure.validate,r), 'nonfinite camera rejected')
r=route(); r.duration=181
check(not pcall(pure.validate,r), 'oversized preview rejected')
local slow, fast = pure.newSampler(route()),pure.newSampler(route())
for i=0,20 do slow(i/20) end
for i=0,120 do fast(i/120) end
local _, a = slow(1); local _, b=fast(1)
check(near(a.camera.yaw,b.camera.yaw) and near(a.position.x,b.position.x), 'camera result does not accumulate FPS-dependent deltas')

-- Mock the actual PLAYER file for timing, override release, policy drain and errors.
local function playerFixture(seedX, seedCell)
    local clock, position, yaw, pitch, paused = 0,{x=seedX or 0,y=0,z=0},math.pi-0.1,0.1,false
    local calls, messages, events, quit, flags = {},{}, {},false,{movement=false,combat=false,ui=false}
    local actor = { controls={}, ATTACK_TYPE={NoAttack=0},position=position,
        cell={id=seedCell or 'Balmora1',isExterior=true}, rotation={getYaw=function() return yaw end,getPitch=function() return pitch end} }
    local fakeCamera={MODE={FirstPerson=1},getYaw=function() return yaw end,getPitch=function() return pitch end,
        setYaw=function(v) yaw=v end,setPitch=function(v) pitch=v end,setMode=function() end,setFieldOfView=function() end}
    local fakeCore={getSimulationTime=function() return clock end,getRealTime=function() return clock end,
        isWorldPaused=function() return paused end,quit=function() quit=true end,
        sendGlobalEvent=function(name,data) events[#events+1]={name=name,data=data} end}
    local interfaces={UI={getMode=function() return nil end},Controls={
        overrideMovementControls=function(v) flags.movement=v end,
        overrideCombatControls=function(v) flags.combat=v end,
        overrideUiControls=function(v) flags.ui=v end}}
    local modules={['openmw.core']=fakeCore,['openmw.self']=actor,['openmw.camera']=fakeCamera,
        ['openmw.input']={KEY={F11=11}},['openmw.ui']={showMessage=function(v) messages[#messages+1]=v end},
        ['openmw.types']={Player={CONTROL_SWITCH={VanityMode=1},setControlSwitch=function() end}},
        ['openmw.interfaces']=interfaces,['scripts.optimizedmw_route_playback.core']=pure,
        ['scripts.optimizedmw_route_playback.options']={enabled=true,session=string.rep('a',32),autostart_seconds=1,
            quit_on_finish=true,position_tolerance=128,abort_position_error=2048,max_duration_seconds=180},
        ['scripts.optimizedmw_route_playback.timeline']=route(),
        ['scripts.optimizedmw_route_recorder.common']={json=function(v) calls[#calls+1]=v; return v.event end}}
    for name,value in pairs(modules) do package.loaded[name]=value end
    local beforePrint=print
    print=function() end
    local handlers=dofile(root..'/scripts/optimizedmw_route_playback/player.lua')
    print=beforePrint
    local function invoke(dt)
        local oldPrint=print; print=function() end
        handlers.engineHandlers.onFrame(dt)
        print=oldPrint
    end
    return { actor=actor,flags=flags,calls=calls,events=events,
        tick=function(t,dt) clock=t; invoke(dt) end,
        policy=function(event)
            local oldPrint=print; print=function() end
            handlers.eventHandlers.OptimizedMWRoutePolicy(event)
            print=oldPrint
        end,
        abort=function()
            local oldPrint=print; print=function() end
            handlers.engineHandlers.onKeyPress({code=11})
            print=oldPrint
        end,
        isQuit=function() return quit end,
        pause=function(v) paused=v end }
end
local function ready(f)
    f.policy({session=string.rep('a',32),event='policy_ready',ready=true,qualified=true,pending_disable=0,failed=false})
end
local function lastEvent(f,name)
    for i=#f.calls,1,-1 do if f.calls[i].event==name then return f.calls[i] end end
end
local f=playerFixture()
f.tick(0,0.5)
check(f.flags.movement and f.flags.combat and not lastEvent(f,'start'), 'holds inputs while policy is unready')
ready(f); f.tick(0.5,0.5)
check(not lastEvent(f,'start'), 'waits configured unpaused time')
f.tick(1,0.5)
check(lastEvent(f,'start') and f.flags.ui, 'qualified setup begins automatic playback')
f.tick(2,1)
check(f.actor.controls.movement==1, 'native actor movement receives recorded input')
f.tick(3,1)
check(f.actor.controls.movement==0 and not f.isQuit(), 'completion holds still and awaits observer drain')
f.policy({session=string.rep('a',32),event='policy_stopped',ready=true,qualified=true})
for i=1,4 do f.tick(3+i*0.3,0.3) end
check(f.isQuit() and not f.flags.movement and not f.flags.combat and not f.flags.ui, 'completion releases overrides before quit')
check(lastEvent(f,'finish').reason=='route_finished' and lastEvent(f,'finish').benchmark_qualified==false,
    'finished input replay does not claim benchmark acceptance')
f=playerFixture(200); ready(f); f.tick(0,1)
check(f.actor.controls.movement==0 and not lastEvent(f,'start'), 'bad initial position cannot start playback')
for i=1,7 do f.tick(i,1) end
check(lastEvent(f,'finish').reason=='starting_position_mismatch', 'bad initial position reports specific failure')
f=playerFixture(0,'WrongCell'); ready(f); f.tick(0,1)
for i=1,7 do f.tick(i,1) end
check(lastEvent(f,'finish').reason=='starting_cell_mismatch', 'bad initial cell cannot start playback')
f=playerFixture(); ready(f); f.tick(0,1); f.actor.position.x=3000; f.tick(0.3,0.3)
for i=1,7 do f.tick(i,1) end
check(lastEvent(f,'finish').reason=='position_divergence', 'large path divergence aborts instead of teleporting')
f=playerFixture(); ready(f); f.tick(0,1); f.abort()
for i=1,7 do f.tick(i,1) end
check(lastEvent(f,'finish').reason=='user_aborted', 'bare F11 abort reports cancellation')
f=playerFixture(); ready(f); f.tick(0,1); f.pause(true); f.tick(0.1,0)
for i=1,7 do f.tick(i,0) end
check(lastEvent(f,'finish').reason=='unexpected_pause', 'unexpected UI/world pause invalidates replay')
f=playerFixture(); ready(f); f.tick(0,1); f.tick(2,2)
f.policy({session=string.rep('a',32),event='policy_stopped',ready=true,qualified=true,combat_during_recording=1})
for i=1,4 do f.tick(2+i*0.3,0.3) end
check(lastEvent(f,'finish').reason=='combat_observed' and not lastEvent(f,'finish').input_replay_finished,
    'final policy combat observation invalidates completed playback')
f=playerFixture(); ready(f); f.tick(0,1); f.tick(2,2)
for i=1,7 do f.tick(2+i,1) end
check(lastEvent(f,'finish').reason=='observer_drain_timeout' and not lastEvent(f,'finish').input_replay_finished,
    'missing final observer acknowledgement cannot claim finished replay')
f=playerFixture(); ready(f); f.tick(0,1); f.tick(2,2)
f.policy({session=string.rep('a',32),event='policy_stopped',ready=false,qualified=false,pending_disable=1})
for i=1,7 do f.tick(2+i,1) end
check(lastEvent(f,'finish').reason=='hostile_policy_pending_at_exit' and not lastEvent(f,'finish').policy_observer_drained,
    'pending final disable confirmation cannot claim drained observer')
print('Playback core and PLAYER mock checks passed: '..checks)
