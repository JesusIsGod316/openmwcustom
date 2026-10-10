local root = assert(arg[1], 'package directory required')
local pure = dofile(root .. '/scripts/optimizedmw_route_playback/core.lua')
local guide = dofile(root .. '/scripts/optimizedmw_route_playback/guidance.lua')
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
local current, future = pure.newSampler(route()), pure.newCommandSampler(route())
local command, commandTime = future(0.4,0.6)
check(near(commandTime,1) and near(command.camera.yaw,-math.pi+0.1), 'command samples end-of-frame heading across angle wrap')
c,p = current(0.4)
check(c.movement==0 and near(p.position.x,4), 'future heading does not advance current movement or position sampling')
c,p = current(0.8)
check(c.movement==0 and near(p.position.x,8), 'current input cursor remains before future movement boundary')
command,commandTime = future(1.9,0.4)
check(commandTime==2 and near(command.position.x,20), 'command horizon clamps at route duration')
command,commandTime = future(0.4,0.5)
command,commandTime = future(0.41,0.01)
check(near(commandTime,0.42) and near(command.position.x,4.2), 'variable frame duration does not retain a stale future horizon')
local futureSlow, futureFast = pure.newCommandSampler(route()),pure.newCommandSampler(route())
for i=0,4 do futureSlow(i*0.2,0.2) end
for i=0,99 do futureFast(i*0.01,0.01) end
local endSlow,timeSlow=futureSlow(0.8,0.2); local endFast,timeFast=futureFast(0.99,0.01)
check(near(timeSlow,timeFast) and near(endSlow.camera.yaw,endFast.camera.yaw), 'different update rates select identical heading for identical endpoint time')
check(not pcall(future,0,-0.1), 'negative command delta rejected')
check(not pcall(future,0,0/0), 'nonfinite command delta rejected')

local forward,side=guide.worldToLocal(3,4,0)
check(near(forward,4) and near(side,3), 'native yaw-zero basis projects world +Y forward and +X right')
forward,side=guide.worldToLocal(3,4,math.pi/2)
check(near(forward,3) and near(side,-4), 'quarter-turn actor basis preserves correction signs')
local wx,wy=guide.localToWorld(forward,side,math.pi/2)
check(near(wx,3) and near(wy,4), 'actor-local projection round trips to world motion')
local origin={x=0,y=0,z=0}
local applied=guide.command(origin,{x=1000,y=1000,z=0},input(0,1),0,100,false)
check(applied.correction_norm<=0.350000001 and applied.input_norm<=1.000000001,
    'large physical feedback remains bounded independently and after nominal input')
applied=guide.command(origin,{x=0,y=40,z=0},input(0,1),0,100,true)
check(applied.nominal_movement==0 and applied.movement>0 and applied.input_norm<=0.350000001,
    'held reference drops nominal forward motion while bounded feedback catches up')
applied=guide.command(origin,{x=20,y=20,z=0},input(0,0),0,100,false)
check(applied.movement==0 and applied.side_movement==0 and applied.stationary_source,
    'recorded fixed-view and stop inputs do not acquire corrective motion')
applied=guide.setupCommand(origin,{x=100,y=100,z=0},0,100)
check(applied.input_norm<=0.200000001, 'initial alignment has its separate slower input cap')
check(not pcall(guide.setupCommand,origin,origin,0,0), 'missing gait speed does not invent physical velocity')
local clock=guide.newClock(10)
check(clock:advance(0.1)==0.1, 'reference clock advances the elapsed prior physics interval')
local held,changed=clock:updateHold(33)
check(held and changed and clock:futureDelta(0.1)==0, 'corridor excess holds the current reference target')
local ref,lag=clock:advance(0.4)
check(near(ref,0.1) and near(lag,0.3) and near(clock.hold_seconds,0.3),
    'held clock is monotone and preserves explicit source/reference lag')
held,changed=clock:updateHold(20)
check(held and not changed, 'hold hysteresis does not resume between thresholds')
held,changed=clock:updateHold(15)
check(not held and changed and near(clock:futureDelta(0.05),0.05), 'recovered corridor resumes only below exit threshold')
ref,lag=clock:advance(0.5)
check(near(ref,0.2) and near(lag,0.3), 'reference resumes without an unbounded time jump')
check(not pcall(clock.advance,clock,0.4), 'guided clock rejects reversal')
local recovery={x=0,y=-60,z=0}
local recoveryClock=guide.newClock(10)
recoveryClock:updateHold(60)
for i=1,360 do
    local cmd=guide.command(recovery,origin,input(0,1),0,100,recoveryClock.held)
    local dx,dy=guide.localToWorld(cmd.movement,cmd.side_movement,0)
    recovery.x,recovery.y=recovery.x+dx*100/120,recovery.y+dy*100/120
    recoveryClock:advance(i/120)
    local xy=guide.errors(recovery,origin)
    if xy<16 then recoveryClock:updateHold(xy); break end
end
check(guide.errors(recovery,origin)<16 and recovery.y>-60 and recoveryClock.reference==0,
    'ordinary physics feedback closes XY error while the target clock stays held')
local stall=guide.newStallGuard()
check(not stall:observe(0,origin,0.5) and not stall:observe(1.9,{x=7,y=0,z=0},0.5), 'stall requires a full bounded observation window')
check(stall:observe(2,{x=7,y=0,z=0},0.5), 'commanded motion without eight units progress fails after two seconds')
stall=guide.newStallGuard(); stall:observe(0,origin,0.5)
check(not stall:observe(2,{x=8,y=0,z=0},0.5), 'actual physical progress resets the stall window')
stall:observe(3,{x=8,y=0,z=0},0)
check(not stall:observe(5,{x=8,y=0,z=0},0.5), 'recorded stops do not inherit a stale stall window')
check(guide.finalPoseAccepted({x=24,y=0,z=48},origin), 'final corridor permits its inclusive XY/Z bounds')
check(not guide.finalPoseAccepted({x=25,y=0,z=0},origin) and not guide.finalPoseAccepted({x=0,y=0,z=49},origin),
    'reference time alone cannot accept an inaccurate endpoint')

-- Mock the actual PLAYER file for timing, override release, policy drain and errors.
local function playerFixture(seedX, seedCell, fixtureOptions, fixtureRoute)
    fixtureOptions=fixtureOptions or {}; fixtureRoute=fixtureRoute or route()
    local clock, position, yaw, pitch, paused = 0,{x=seedX or 0,y=0,z=0},math.pi-0.1,0.1,false
    local actorYaw, actorPitch = fixtureRoute.samples[1].camera.yaw,fixtureRoute.samples[1].camera.pitch
    yaw,pitch=actorYaw,actorPitch
    local collision=true
    local calls, messages, events, quit, flags = {},{}, {},false,{movement=false,combat=false,ui=false}
    local actor = { controls={}, ATTACK_TYPE={NoAttack=0},position=position,
        cell={id=seedCell or 'Balmora1',isExterior=true}, rotation={getYaw=function() return actorYaw end,getPitch=function() return actorPitch end} }
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
        ['openmw.types']={Player={CONTROL_SWITCH={VanityMode=1},setControlSwitch=function() end},
            Actor={getRunSpeed=function() return 100 end,getWalkSpeed=function() return 50 end,
                isOnGround=function() return true end,isSwimming=function() return false end,
                getCurrentSpeed=function() return actor.controls.run and 100 or 50 end}},
        ['openmw.debug']={isCollisionEnabled=function() return collision end},
        ['openmw.interfaces']=interfaces,['scripts.optimizedmw_route_playback.core']=pure,
        ['scripts.optimizedmw_route_playback.guidance']=guide,
        ['scripts.optimizedmw_route_playback.options']={enabled=true,session=string.rep('a',32),autostart_seconds=1,
            quit_on_finish=true,position_tolerance=128,abort_position_error=2048,max_duration_seconds=180},
        ['scripts.optimizedmw_route_playback.timeline']=fixtureRoute,
        ['scripts.optimizedmw_route_recorder.common']={json=function(v) calls[#calls+1]=v; return v.event end}}
    for name,value in pairs(fixtureOptions) do modules['scripts.optimizedmw_route_playback.options'][name]=value end
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
        step=function(dt,blocked)
            invoke(dt)
            actorYaw=actorYaw+(actor.controls.yawChange or 0); actorPitch=actorPitch+(actor.controls.pitchChange or 0)
            actor.controls.yawChange,actor.controls.pitchChange=0,0
            if not blocked then
                local dx,dy=guide.localToWorld(actor.controls.movement or 0,actor.controls.sideMovement or 0,actorYaw)
                local speed=actor.controls.run and 100 or 50
                position.x,position.y=position.x+dx*speed*dt,position.y+dy*speed*dt
            end
            clock=clock+dt
        end,
        collision=function(v) collision=v end,
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
f=playerFixture(); ready(f); f.tick(0,1)
local initialCheck=lastEvent(f,'check')
check(near(initialCheck.command_time,1) and initialCheck.frame_simulation_delta==1,
    'PLAYER observation records commanded endpoint and current simulation delta')
check(f.actor.controls.movement==0 and near(f.actor.controls.yawChange,0.2),
    'PLAYER leads heading without prematurely taking future movement input')
check(initialCheck.actual_actor_yaw_before==initialCheck.actor_yaw_after_write,
    'camera setter readback does not pretend mechanics already rotated actor')
check(near(initialCheck.camera_postwrite_yaw_error,0) and initialCheck.postwrite_readback_phase,
    'postwrite camera readback is separated from pre-command observations')
local function guidedRoute()
    return {schema=1,route_name='balmora',duration=3,
        controls={input(0,0),input(1,1),input(3,0)},
        samples={row(0,0,math.pi/2),row(1,0,math.pi/2),row(2,100,math.pi/2),row(3,200,math.pi/2)},markers={}}
end
local function guidedFinish(f, blocked)
    local acknowledged=false
    for i=1,150 do
        f.step(0.1,blocked)
        for _,event in ipairs(f.events) do
            if event.data.active==false and not acknowledged then
                f.policy({session=string.rep('a',32),event='policy_stopped',ready=true,qualified=true,pending_disable=0})
                acknowledged=true
            end
        end
        if f.isQuit() then return lastEvent(f,'finish') end
    end
    error('Guided fixture did not finish in bounded time')
end
f=playerFixture(-8,nil,{guidance_enabled=true},guidedRoute()); ready(f)
local finish=guidedFinish(f,false)
check(finish.reason=='route_finished' and finish.route_playback_finished and not finish.input_replay_finished,
    'real PLAYER feedback preview completes physical route but never claims raw input replay')
check(lastEvent(f,'alignment_check') and lastEvent(f,'alignment_finished').horizontal_error<=6,
    'PLAYER physically aligns the seed before its fixed-view route begins')
check(lastEvent(f,'start').starting_position.x<0 and lastEvent(f,'start').starting_position.x>=-6,
    'initial alignment uses limited ordinary movement rather than rewriting start position')
check(finish.playback_protocol=='physical_xy_follower_v1' and finish.physical_state.collision_enabled==true
    and finish.benchmark_qualified==false, 'successful guided preview identifies protocol and collision proof without benchmark claim')
local guidedCheck=lastEvent(f,'check')
check(guidedCheck.horizontal_error<=24 and guidedCheck.vertical_error<=48,
    'physical feedback endpoint satisfies the final position corridor')
local detailed
for _,v in ipairs(f.calls) do if v.event=='check' and v.guidance_command then detailed=v end end
check(detailed and detailed.raw_controls and detailed.applied_controls and detailed.reference_time
    and detailed.guidance_command.correction_norm<=0.350000001,
    'PLAYER separately reports acquired input, applied input and bounded corrective work')
f=playerFixture(0,nil,{guidance_enabled=true},guidedRoute()); ready(f)
finish=guidedFinish(f,true)
check(finish.reason=='physical_movement_stalled' and not finish.route_playback_finished,
    'collision-blocked ordinary movement aborts through the actual PLAYER stall guard')
check(lastEvent(f,'reference_hold') and finish.reference_hold_count>=1,
    'stalled physics holds route time explicitly before aborting')
f=playerFixture(0,nil,{guidance_enabled=true},guidedRoute()); f.collision(false); ready(f)
finish=guidedFinish(f,false)
check(finish.reason=='collision_disabled' and finish.physical_state.collision_enabled==false,
    'guided preview refuses collision-off playback and preserves false in telemetry')
f=playerFixture(0,nil,{guidance_enabled=true},guidedRoute()); f.collision(nil); ready(f)
finish=guidedFinish(f,false)
check(finish.reason=='collision_state_unavailable' and not finish.route_playback_finished,
    'missing physical collision proof cannot silently validate guided preview')
f=playerFixture(0,nil,{guidance_enabled=true},guidedRoute()); f.actor.position.z=49; ready(f)
finish=guidedFinish(f,false)
check(finish.reason=='initial_vertical_mismatch', 'guided setup rejects a different vertical level without repositioning')
f=playerFixture(0,nil,{guidance_enabled=true},guidedRoute()); ready(f); f.tick(0,1)
f.actor.position.x=100; f.tick(0.3,0.3)
finish=guidedFinish(f,false)
check(finish.reason=='guided_horizontal_divergence' and not finish.route_playback_finished,
    'guided hard corridor is tighter than raw replay tolerance')
f=playerFixture(0,nil,{guidance_enabled=true},guidedRoute()); ready(f); f.tick(0,1)
f.actor.position.x=40; f.tick(0.3,0.3)
finish=guidedFinish(f,false)
check(finish.reason=='stationary_pose_divergence', 'stationary source segment does not move the character to hide a mismatch')
print('Playback core and PLAYER mock checks passed: '..checks)
