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
    return { t=t, position={x=x,y=0,z=0}, cell_id=cell or 'Esm3ExteriorCell:-2:-2', cell_name='Balmora', exterior=true,
        camera={yaw=yaw,pitch=0.1,fov=1.5,mode=1} }
end
local function input(t, movement)
    return {t=t,movement=movement,side_movement=0,run=true,sneak=false,jump=false,use=0}
end
local function route()
    return {schema=1,route_name='balmora',duration=2,controls={input(0,0),input(1,1),input(1.8,0)},
        samples={row(0,0,math.pi-0.1),row(1,10,-math.pi+0.1),row(2,20,-math.pi+0.3,'Esm3ExteriorCell:-3:-2')},
        markers={{t=1,index=1}}}
end
local lookup = pure.newSampler(route())
local c, p, index = lookup(0.5)
check(c.movement == 0 and index == 1, 'previous control remains until its time')
check(near(p.position.x,5), 'expected pose interpolates by simulation time')
check(near(math.abs(p.camera.yaw),math.pi), 'camera interpolation uses shortest wrap')
check(p.cell_id == 'Esm3ExteriorCell:-2:-2', 'cell does not change before crossing time')
c,p,index = lookup(1)
check(c.movement == 1 and index == 2, 'boundary consumes new input')
c,p = lookup(1.9)
check(c.movement == 0 and near(p.position.x,19), 'skipped frames catch up to current controls')
c,p = lookup(2)
check(p.cell_id == 'Esm3ExteriorCell:-3:-2' and near(p.position.x,20), 'final pose endpoint')
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
local function nativeCohort()
    return {model='native_input_displacement_with_visual_animations',private_override=true}
end
local function existingRoute()
    local value=route()
    value.route_name='existing'; value.movement_cohort=nativeCohort(); value.movement_cohort_status='recorded_native'
    local cells={{id='Esm3ExteriorCell:-2:-9',name='Seyda Neen',x=-2,y=-9},
        {id='Esm3ExteriorCell:-1:-9',name='',x=-1,y=-9},
        {id='Esm3ExteriorCell:0:-8',name='Pelagiad',x=0,y=-8}}
    for i,s in ipairs(value.samples) do
        s.cell_id,s.cell_name,s.grid_x,s.grid_y=cells[i].id,cells[i].name,cells[i].x,cells[i].y
    end
    return value
end
local seyda=pure.newSampler(existingRoute())
local _,seydaPose=seyda(0)
check(seydaPose.cell_id=='Esm3ExteriorCell:-2:-9' and seydaPose.cell_name=='Seyda Neen'
    and seydaPose.grid_x==-2 and seydaPose.grid_y==-9, 'native Seyda route keeps canonical start grid identity')
_,seydaPose=seyda(1)
check(seydaPose.cell_id=='Esm3ExteriorCell:-1:-9' and seydaPose.cell_name=='', 'unnamed exterior identity survives the sampler')
_,seydaPose=seyda(2)
check(seydaPose.cell_id=='Esm3ExteriorCell:0:-8' and seydaPose.cell_name=='Pelagiad'
    and seydaPose.grid_x==0 and seydaPose.grid_y==-8, 'named exterior transition is driven by ID and grid')
r=existingRoute();r.samples[3].cell_id='Esm3ExteriorCell:-1:-8';r.samples[3].grid_x=-1;r.samples[3].cell_name='Andrano Tomb'
check(pcall(pure.validate,r), 'Andrano Tomb exterior label does not imply an interior door transition')
r=existingRoute();r.movement_cohort=nil
check(not pcall(pure.validate,r), 'existing route requires explicit recorded movement semantics')
r=existingRoute();r.movement_cohort.private_override=1
check(not pcall(pure.validate,r), 'numeric private override cannot claim native cohort')
r=existingRoute();r.movement_cohort.model='frozen_animation_movement_setting'
check(not pcall(pure.validate,r), 'existing acquisition cannot silently inherit animation movement')
r=existingRoute();r.movement_cohort_status='legacy_unrecorded'
check(not pcall(pure.validate,r), 'native source cohort cannot be labeled legacy')
r=route();r.movement_cohort_status='legacy_unrecorded'
check(pcall(pure.validate,r), 'old grid-less Balmora timeline remains explicitly legacy-compatible')
local function animationRoute()
    local value=existingRoute()
    value.movement_cohort={model='animation_displacement',private_override=false}
    value.movement_cohort_status='recorded_animation'
    return value
end
check(pcall(pure.validate,animationRoute()), 'explicitly compiled animation-displacement cohort permits a private Seyda preview')
local animationLookup=pure.newSampler(animationRoute())
local animationInput,animationPose=animationLookup(1)
check(animationInput.movement==1 and near(animationPose.position.x,10)
    and animationPose.cell_id=='Esm3ExteriorCell:-1:-9', 'animation preview retains recorded inputs and canonical cell transitions')
local animationCommand=pure.newCommandSampler(animationRoute())
local animationFuture,animationTime=animationCommand(1.8,0.3)
check(animationTime==2 and animationFuture.cell_id=='Esm3ExteriorCell:0:-8', 'animation preview uses the same bounded camera command sampler')
for _,badStatus in ipairs({'recorded_native','legacy_unrecorded','unknown',true,1}) do
    r=animationRoute();r.movement_cohort_status=badStatus
    check(not pcall(pure.validate,r), 'animation source rejects mismatched or non-string preview status: '..tostring(badStatus))
end
r=animationRoute();r.movement_cohort_status=nil
check(not pcall(pure.validate,r), 'animation model alone does not grant runtime preview consent')
for _,badOverride in ipairs({true,0,1,'false'}) do
    r=animationRoute();r.movement_cohort.private_override=badOverride
    check(not pcall(pure.validate,r), 'animation source requires exact false override: '..tostring(badOverride))
end
r=animationRoute();r.movement_cohort.private_override=nil
check(not pcall(pure.validate,r), 'animation source cannot omit its false override')
r=animationRoute();r.movement_cohort.allow_animation_source=true
check(not pcall(pure.validate,r), 'runtime cohort cannot replace compiled status with an extra consent flag')
r=animationRoute();r.movement_cohort.model='unknown'
check(not pcall(pure.validate,r), 'explicit preview status does not admit unknown movement models')
r=animationRoute();r.movement_cohort=nil
check(not pcall(pure.validate,r), 'animation preview status does not admit a missing Seyda cohort')
r=existingRoute();r.movement_cohort_status='recorded_animation'
check(not pcall(pure.validate,r), 'native cohort cannot be mislabeled animation')
r=existingRoute();r.movement_cohort_status=nil
check(pcall(pure.validate,r), 'native cohort retains its previously accepted optional status')
r=route();r.movement_cohort_status='recorded_animation'
check(not pcall(pure.validate,r), 'legacy Balmora cannot claim a recorded animation cohort without metadata')
r=route();r.route_name='suran'
check(not pcall(pure.validate,r), 'unknown route cohort is rejected')
for _,badId in ipairs({'Balmora1','Esm3ExteriorCell:-02:-2','Esm3ExteriorCell:-0:-2',
        'Esm3ExteriorCell:+2:-2','Esm3ExteriorCell:2147483648:-2'}) do
    r=route();r.samples[1].cell_id=badId
    check(not pcall(pure.validate,r), 'noncanonical exterior ID rejected: '..badId)
end
r=existingRoute();r.samples[1].grid_x=-1
check(not pcall(pure.validate,r), 'mismatched exterior grid rejected independently of display name')
r=existingRoute();r.samples[1].grid_y=nil
check(not pcall(pure.validate,r), 'partial exterior grid metadata rejected')
r=route();r.samples[1].camera.extra_yaw=0.1
check(not pcall(pure.validate,r), 'extra camera rotation layer remains unsupported')
local function layeredRoute()
    local value=route()
    value.camera_layer_cohort={model='recorded_additive_first_person_v1'}
    value.camera_layer_cohort_status='recorded_additive'
    for i,s in ipairs(value.samples) do
        s.camera.extra_yaw,s.camera.extra_pitch,s.camera.roll,s.camera.extra_roll=0.1*i,-0.2*i,0.3*i,-0.4*i
        s.camera.first_person_offset={x=i,y=-2*i,z=3*i}
    end
    return value
end
local layeredLookup=pure.newSampler(layeredRoute())
local _,layeredPose=layeredLookup(0.5)
check(near(layeredPose.camera.extra_yaw,0.15) and near(layeredPose.camera.extra_pitch,-0.3)
    and near(layeredPose.camera.roll,0.45) and near(layeredPose.camera.extra_roll,-0.6),
    'explicit camera contract interpolates all four angles as separate sampled channels')
check(near(layeredPose.camera.first_person_offset.x,1.5) and near(layeredPose.camera.first_person_offset.y,-3)
    and near(layeredPose.camera.first_person_offset.z,4.5), 'first-person offset interpolates each component')
check(near(math.abs(layeredPose.camera.yaw),math.pi), 'additive yaw does not alter main actor heading')
local layeredCommand=pure.newCommandSampler(layeredRoute())
local layeredEndpoint,layeredTime=layeredCommand(0.4,0.6)
check(layeredTime==1 and near(layeredEndpoint.camera.extra_roll,-0.8), 'future camera command includes additive endpoint channels')
for _,key in ipairs({'extra_yaw','extra_pitch','roll','extra_roll'}) do
    r=layeredRoute();r.samples[1].camera[key]=nil
    check(not pcall(pure.validate,r), 'new camera contract requires '..key)
    r=layeredRoute();r.samples[1].camera[key]=math.pi+0.0001
    check(not pcall(pure.validate,r), 'new camera contract bounds '..key)
    r=layeredRoute();r.samples[1].camera[key]=0/0
    check(not pcall(pure.validate,r), 'new camera contract rejects nonfinite '..key)
end
r=layeredRoute();r.samples[1].camera.first_person_offset=nil
check(not pcall(pure.validate,r), 'new camera contract requires offset')
r=layeredRoute();r.samples[1].camera.first_person_offset.x=1024.01
check(not pcall(pure.validate,r), 'new camera contract bounds offset')
r=layeredRoute();r.samples[1].camera.first_person_offset.x=0/0
check(not pcall(pure.validate,r), 'new camera contract rejects nonfinite offset')
r=layeredRoute();r.samples[1].camera.first_person_offset.w=0
check(not pcall(pure.validate,r), 'new camera contract rejects extra offset component')
r=layeredRoute();r.camera_layer_cohort.model='unknown'
check(not pcall(pure.validate,r), 'unknown camera model cannot admit layers')
r=layeredRoute();r.camera_layer_cohort.allow_any=true
check(not pcall(pure.validate,r), 'unknown camera cohort fields cannot admit layers')
r=layeredRoute();r.camera_layer_cohort_status='legacy_zero_layers'
check(not pcall(pure.validate,r), 'camera profile cannot claim legacy status')
r=route();r.samples[1].camera.extra_roll=0.1
check(not pcall(pure.validate,r), 'legacy path cannot discard nonzero extra roll')
r=route();r.samples[1].camera.first_person_offset={x=0,y=0,z=1}
check(not pcall(pure.validate,r), 'legacy path cannot discard nonzero first-person offset')
r=existingRoute();r.samples[1].exterior=false
check(not pcall(pure.validate,r), 'existing route does not bypass the interior transition guard')
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
check(applied.stationary_recovery_active==false, 'legacy stationary command does not claim recovery work')
applied=guide.command(origin,{x=20,y=20,z=0},input(0,0),0,100,true,false)
check(applied.input_norm==0 and not applied.stationary_recovery_active, 'explicitly disabled held stationary recovery stays zero')
applied=guide.command(origin,{x=1000,y=1000,z=0},input(0,0),0,100,true,true)
check(applied.stationary_recovery_active and applied.stationary_source and applied.nominal_movement==0
    and applied.nominal_side_movement==0 and applied.correction_norm<=0.350000001 and applied.input_norm<=0.350000001,
    'opted stationary held correction uses only the existing bounded XY feedback')
applied=guide.command(origin,{x=0,y=12,z=0},input(0,0),0,100,false,true)
check(applied.stationary_recovery_active and near(applied.movement,0.24) and applied.side_movement==0,
    'opted stationary correction can settle small error before a reference hold is needed')
applied=guide.setupCommand(origin,{x=100,y=100,z=0},0,100)
check(applied.input_norm<=0.200000001, 'initial alignment has its separate slower input cap')
check(not pcall(guide.setupCommand,origin,origin,0,0), 'missing gait speed does not invent physical velocity')
local clock=guide.newClock(10)
check(not clock.movement_started and not clock:observeMovement(input(0,0)), 'initial fixed-view source never enables post-motion recovery')
check(clock:observeMovement(input(1,1)) and clock:observeMovement(input(2,0)), 'recorded movement enables recovery for later stops without resetting at zero')
local sideClock=guide.newClock(10);local sideInput=input(0,0);sideInput.side_movement=-1
check(sideClock:observeMovement(sideInput), 'recorded side movement also establishes the post-motion phase')
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
local function playerFixture(seedX, seedCell, fixtureOptions, fixtureRoute, fixturePhysics)
    fixtureOptions=fixtureOptions or {}; fixtureRoute=fixtureRoute or route()
    local clock, position, yaw, pitch, paused = 0,{x=seedX or 0,y=0,z=0},math.pi-0.1,0.1,false
    local actorYaw, actorPitch = fixtureRoute.samples[1].camera.yaw,fixtureRoute.samples[1].camera.pitch
    yaw,pitch=actorYaw,actorPitch
    local collision=true
    local velocityX,velocityY=0,0
    local calls, messages, events, quit, flags = {},{}, {},false,{movement=false,combat=false,ui=false}
    local actor = { controls={}, ATTACK_TYPE={NoAttack=0},position=position,
        cell={id=seedCell or 'Esm3ExteriorCell:-2:-2',isExterior=true}, rotation={getYaw=function() return actorYaw end,getPitch=function() return actorPitch end} }
    local layers={extra_yaw=0.01,extra_pitch=0.02,roll=0.03,extra_roll=0.04,first_person_offset={x=4,y=5,z=6}}
    local layerWrites=0
    local fakeCamera={MODE={FirstPerson=1},getYaw=function() return yaw end,getPitch=function() return pitch end,
        setYaw=function(v) yaw=v end,setPitch=function(v) pitch=v end,setMode=function() end,setFieldOfView=function() end}
    for key,suffix in pairs({extra_yaw='ExtraYaw',extra_pitch='ExtraPitch',roll='Roll',extra_roll='ExtraRoll'}) do
        local field=key
        fakeCamera['get'..suffix]=function() return layers[field] end
        fakeCamera['set'..suffix]=function(v) layers[field]=v;layerWrites=layerWrites+1 end
    end
    fakeCamera.getFirstPersonOffset=function() return layers.first_person_offset end
    fakeCamera.setFirstPersonOffset=function(v) layers.first_person_offset=v;layerWrites=layerWrites+1 end
    local fakeCore={getSimulationTime=function() return clock end,getRealTime=function() return clock end,
        isWorldPaused=function() return paused end,quit=function() quit=true end,
        sendGlobalEvent=function(name,data) events[#events+1]={name=name,data=data} end}
    local interfaces={UI={getMode=function() return nil end},Controls={
        overrideMovementControls=function(v) flags.movement=v end,
        overrideCombatControls=function(v) flags.combat=v end,
        overrideUiControls=function(v) flags.ui=v end}}
    local modules={['openmw.core']=fakeCore,['openmw.self']=actor,['openmw.camera']=fakeCamera,
        ['openmw.util']={vector3=function(x,y,z) return {x=x,y=y,z=z} end},
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
    if fixtureRoute.camera_layer_cohort then modules['scripts.optimizedmw_route_playback.options'].camera_layer_cohort=fixtureRoute.camera_layer_cohort end
    for name,value in pairs(modules) do package.loaded[name]=value end
    local beforePrint=print
    print=function() end
    local loaded,handlers=pcall(dofile,root..'/scripts/optimizedmw_route_playback/player.lua')
    print=beforePrint
    assert(loaded,handlers)
    local function invoke(dt)
        local oldPrint=print; print=function() end
        handlers.engineHandlers.onFrame(dt)
        print=oldPrint
    end
    return { actor=actor,flags=flags,calls=calls,events=events,camera=fakeCamera,layers=layers,
        layerWrites=function() return layerWrites end,
        tick=function(t,dt) clock=t; invoke(dt) end,
        step=function(dt,blocked)
            invoke(dt)
            actorYaw=actorYaw+(actor.controls.yawChange or 0); actorPitch=actorPitch+(actor.controls.pitchChange or 0)
            actor.controls.yawChange,actor.controls.pitchChange=0,0
            if not blocked then
                local dx,dy=guide.localToWorld(actor.controls.movement or 0,actor.controls.sideMovement or 0,actorYaw)
                local speed=actor.controls.run and 100 or 50
                if fixturePhysics and fixturePhysics.inertia_seconds then
                    local alpha=dt/(fixturePhysics.inertia_seconds+dt)
                    velocityX,velocityY=velocityX+(dx*speed-velocityX)*alpha,velocityY+(dy*speed-velocityY)*alpha
                    dx,dy,speed=velocityX,velocityY,1
                end
                position.x,position.y=position.x+dx*speed*dt,position.y+dy*speed*dt
            else
                velocityX,velocityY=0,0
            end
            clock=clock+dt
        end,
        collision=function(v) collision=v end,
        velocity=function(x,y) velocityX,velocityY=x,y end,
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
check(f.layerWrites()==0 and near(f.layers.extra_pitch,0.02), 'legacy Balmora never acquires or writes additive camera channels')
local animationFixtureRoute=animationRoute()
f=playerFixture(0,animationFixtureRoute.samples[1].cell_id,nil,animationFixtureRoute)
ready(f);f.tick(0,0.5);f.tick(1,0.5)
check(lastEvent(f,'start') and f.flags.movement, 'actual PLAYER starts an explicitly compiled animation route')
f.tick(2,1)
check(f.actor.controls.movement==1, 'actual PLAYER commands recorded input for animation-displacement preview')
f.tick(3,1)
f.policy({session=string.rep('a',32),event='policy_stopped',ready=true,qualified=true})
for i=1,4 do f.tick(3+i*0.3,0.3) end
check(f.isQuit() and lastEvent(f,'finish').reason=='route_finished'
    and lastEvent(f,'finish').benchmark_qualified==false, 'animation preview completes through actual PLAYER without claiming benchmark acceptance')
check(not f.flags.movement and not f.flags.combat and not f.flags.ui and f.layerWrites()==0,
    'animation preview releases controls and preserves legacy zero-layer camera ownership')
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
for _,badOption in ipairs({'true',0,1}) do
    check(not pcall(playerFixture,0,nil,{guidance_enabled=true,stationary_recovery_enabled=badOption},guidedRoute()),
        'PLAYER rejects nonboolean stationary recovery option: '..tostring(badOption))
end
f=playerFixture(0,nil,{guidance_enabled=true,stationary_recovery_enabled=true},guidedRoute()); ready(f); f.tick(0,1)
f.actor.position.x=40; f.tick(0.3,0.3)
finish=guidedFinish(f,false)
check(finish.reason=='stationary_pose_divergence' and not finish.source_movement_started,
    'opted recovery cannot conceal initial fixed-view divergence before any recorded movement')
check(finish.last_guidance_command.input_norm==0 and not finish.last_guidance_command.stationary_recovery_active,
    'initial fixed view stays physically still even when the private option is enabled')
local function stationaryRoute(duration,withLayers)
    duration=duration or 6
    local value={schema=1,route_name='balmora',duration=duration,
        controls={input(0,0),input(1,1),input(2,0)},
        samples={row(0,0,math.pi/2),row(1,0,math.pi/2),row(2,100,math.pi/2),row(duration,100,math.pi/2)},markers={}}
    if withLayers then
        value.movement_cohort={model='animation_displacement',private_override=false}
        value.movement_cohort_status='recorded_animation'
        value.camera_layer_cohort={model='recorded_additive_first_person_v1'}
        value.camera_layer_cohort_status='recorded_additive'
        for i,s in ipairs(value.samples) do
            s.camera.extra_yaw,s.camera.extra_pitch,s.camera.roll,s.camera.extra_roll=0.1*i,-0.2*i,0.3*i,-0.4*i
            s.camera.first_person_offset={x=i,y=-2*i,z=3*i}
        end
    end
    return value
end
local function postMotionFixture(recovery,withLayers,physics)
    local result=playerFixture(0,nil,{guidance_enabled=true,stationary_recovery_enabled=recovery},stationaryRoute(nil,withLayers),physics)
    ready(result);result.tick(0,1);result.tick(1,0.1)
    result.actor.position.x=160
    result.tick(2,0.1)
    return result
end
f=postMotionFixture(false)
finish=guidedFinish(f,false)
check(finish.reason=='stationary_pose_divergence' and finish.source_movement_started
    and not finish.stationary_recovery_enabled, 'default post-motion stop still rejects divergence without movement correction')
f=postMotionFixture(true)
check(f.actor.controls.movement<0 and math.abs(f.actor.controls.movement)<=0.350000001,
    'opted post-motion overshoot commands bounded physical movement back toward the stationary target')
finish=guidedFinish(f,false)
check(finish.reason=='route_finished' and finish.route_playback_finished and not finish.input_replay_finished
    and finish.stationary_recovery_enabled and finish.source_movement_started,
    'actual PLAYER recovers a held post-motion stop and finishes with explicit private provenance')
check(finish.maximum_horizontal_error<=96 and finish.maximum_vertical_error<=96
    and finish.physical_state.collision_enabled and guide.finalPoseAccepted(f.actor.position,{x=100,y=0,z=0}),
    'stationary recovery retains collision and hard/final pose corridors')
local recoveredCommand
for _,v in ipairs(f.calls) do
    if v.guidance_command and v.guidance_command.stationary_recovery_active then recoveredCommand=v.guidance_command end
end
check(recoveredCommand and recoveredCommand.nominal_movement==0 and recoveredCommand.nominal_side_movement==0
    and recoveredCommand.correction_norm<=0.350000001 and finish.reference_hold_count>=1,
    'stationary recovery telemetry distinguishes zero source input from bounded applied correction and a held clock')
f=postMotionFixture(true)
finish=guidedFinish(f,true)
check(finish.reason=='physical_movement_stalled' and not finish.route_playback_finished,
    'collision-blocked stationary recovery still fails through the original physical stall guard')
f=postMotionFixture(true);f.collision(false);f.tick(2.1,0.1)
finish=guidedFinish(f,false)
check(finish.reason=='collision_disabled' and not finish.route_playback_finished,
    'post-motion recovery never bypasses the collision gate')
f=postMotionFixture(true);f.actor.position.x=197;f.tick(2.1,0.1)
finish=guidedFinish(f,false)
check(finish.reason=='guided_horizontal_divergence', 'stationary recovery cannot widen the 96-unit hard corridor')
f=postMotionFixture(true);f.actor.position.x=100;f.actor.position.z=49;f.tick(6,0.1)
finish=guidedFinish(f,false)
check(finish.reason=='final_pose_mismatch', 'post-motion recovery retains the 48-unit final vertical corridor')
f=playerFixture(0,nil,{guidance_enabled=true,stationary_recovery_enabled=true},stationaryRoute(20));ready(f)
f.tick(0,1);f.tick(1,0.1);f.actor.position.x=160;f.tick(2,0.1)
for i=1,16 do
    -- Physical movement between observations defeats the stall guard, while
    -- neither position reaches the 16-unit reference-release corridor.
    f.actor.position.x=i%2==0 and 160 or 140
    f.tick(2+i,1)
end
finish=guidedFinish(f,false)
check(finish.reason=='reference_clock_lag' and finish.reference_clock_lag>15,
    'stationary recovery never waits indefinitely after the existing source-lag limit')
f=playerFixture(0,nil,{guidance_enabled=true,stationary_recovery_enabled=true},stationaryRoute(nil,true),{inertia_seconds=0.4});ready(f)
f.tick(0,1);f.tick(1,0.1);f.actor.position.x,f.actor.position.y=112,8;f.velocity(80,10);f.tick(2,0.1)
finish=guidedFinish(f,false)
check(finish.reason=='route_finished' and guide.finalPoseAccepted(f.actor.position,{x=100,y=0,z=0})
    and guide.finalPoseAccepted(finish.final_actual_position,{x=100,y=0,z=0})
    and finish.maximum_horizontal_error<=96 and finish.reference_clock_lag<=15,
    '2D inertial stop overshoot settles through the actual PLAYER command/physics loop before terminal acceptance')
check(finish.camera_layer_cohort_status=='recorded_additive' and finish.camera_layers_restored
    and finish.maximum_camera_layer_postwrite_error==0 and finish.maximum_first_person_offset_postwrite_error==0,
    'recovered layered animation-source preview preserves readback and release diagnostics through its finish footer')
check(not f.flags.movement and not f.flags.combat and not f.flags.ui and not finish.benchmark_qualified
    and near(f.layers.extra_pitch,0.02), 'stationary recovery completion releases private controls/camera and remains preview-only')
f=playerFixture(0,nil,{guidance_enabled=true,stationary_recovery_enabled=true},stationaryRoute(2.01,true),{inertia_seconds=0.4});ready(f)
f.tick(0,1);f.tick(1,0.1);f.actor.position.x=90;f.velocity(100,0);f.tick(2,0.01)
check(guide.finalPoseAccepted(f.actor.position,{x=100,y=0,z=0}), 'short-stop reproduction is inside the final corridor before observer drain')
f.tick(2.01,0.01);finish=guidedFinish(f,false)
check(finish.reason=='final_pose_mismatch' and not finish.route_playback_finished
    and not guide.finalPoseAccepted(finish.final_actual_position,{x=100,y=0,z=0}),
    'residual inertia crossing the 24-unit corridor during shutdown drain cannot emit a successful guided footer')
check(finish.camera_layers_restored and not f.flags.movement and not f.flags.combat and not f.flags.ui,
    'post-drain final pose rejection still restores additive camera state and private controls')
f=playerFixture(0,nil,{guidance_enabled=true,stationary_recovery_enabled=true},stationaryRoute(2.01,true),{inertia_seconds=0.4});ready(f)
f.tick(0,1);f.tick(1,0.1);f.actor.position.x=100;f.velocity(0,0);f.tick(2,0.01);f.tick(2.01,0.01)
finish=guidedFinish(f,false)
check(finish.reason=='route_finished' and finish.route_playback_finished
    and guide.finalPoseAccepted(finish.final_actual_position,{x=100,y=0,z=0}) and finish.camera_layers_restored,
    'settled short-stop control retains successful guided completion after observer drain')
f=playerFixture(0,nil,{guidance_enabled=false,position_tolerance=32,abort_position_error=96},stationaryRoute(2.01),{inertia_seconds=0.4});ready(f)
f.tick(0,1);f.tick(1,0.1);f.actor.position.x=90;f.velocity(100,0);f.tick(2,0.01);f.tick(2.01,0.01)
finish=guidedFinish(f,false)
check(finish.reason=='route_finished' and finish.input_replay_finished
    and not guide.finalPoseAccepted(finish.final_actual_position,{x=100,y=0,z=0}),
    'guided post-drain endpoint gate does not replace the raw diagnostic protocol existing position tolerance')
f=playerFixture(0,nil,{stationary_recovery_enabled=true},stationaryRoute());ready(f);f.tick(0,1);f.tick(1,0.1)
f.actor.position.x=160;f.tick(2,0.1)
check(f.actor.controls.movement==0 and f.actor.controls.sideMovement==0,
    'the stationary recovery option alone cannot add guidance to the raw diagnostic control')
f=playerFixture(0,nil,nil,layeredRoute()); ready(f); f.tick(0,1)
local cameraCheck=lastEvent(f,'check')
check(cameraCheck.camera_layer_cohort_status=='recorded_additive' and cameraCheck.camera_layer_cohort.model=='recorded_additive_first_person_v1',
    'PLAYER identifies its explicit camera contract on observations')
check(near(f.layers.extra_yaw,0.2) and near(f.layers.extra_pitch,-0.4) and near(f.layers.roll,0.6)
    and near(f.layers.extra_roll,-0.8) and near(f.layers.first_person_offset.z,6),
    'PLAYER applies all recorded additive channels through public setters')
check(near(f.actor.controls.yawChange,0.2), 'PLAYER keeps extra yaw separate from actor mechanics')
check(cameraCheck.camera_layer_postwrite_errors.extra_pitch==0 and cameraCheck.first_person_offset_postwrite_error==0,
    'PLAYER reports additive camera setter readback errors')
f.layers.extra_pitch=1 -- represent earlier live mod work before this frame's last harness handler
f.tick(0.1,0.1)
check(f.layers.extra_pitch~=1, 'recorded profile replaces the earlier mod result instead of double-adding it')
f.abort()
for i=1,7 do f.tick(i,1) end
check(lastEvent(f,'finish').reason=='user_aborted' and lastEvent(f,'finish').camera_layers_restored==true,
    'abort releases owned additive camera state')
check(near(f.layers.extra_yaw,0.01) and near(f.layers.extra_pitch,0.02) and near(f.layers.roll,0.03)
    and near(f.layers.extra_roll,0.04) and f.layers.first_person_offset.x==4 and f.layers.first_person_offset.y==5
    and f.layers.first_person_offset.z==6, 'release restores the original private values for every camera channel')
f=playerFixture(0,nil,nil,layeredRoute());f.camera.setExtraRoll=nil;ready(f);f.tick(0,1)
check(f.layerWrites()==0 and not f.flags.movement and not f.flags.combat,
    'missing installed camera setter fails before acquiring overrides or making partial writes')
for i=1,7 do f.tick(i,1) end
check(lastEvent(f,'finish').reason=='playback_error', 'missing camera API terminates with explicit failed preview')
f=playerFixture(0,nil,nil,layeredRoute());ready(f);f.tick(0,1)
f.camera.setExtraPitch=function() error('injected partial camera setter failure') end
f.tick(0.1,0.1)
f.camera.setExtraPitch=function(v) f.layers.extra_pitch=v end
for i=1,7 do f.tick(i,1) end
check(lastEvent(f,'finish').reason=='playback_error' and near(f.layers.extra_yaw,0.01)
    and near(f.layers.extra_pitch,0.02), 'partial write failure still restores owned camera channels')
f=playerFixture(0,nil,nil,layeredRoute());ready(f)
local originalSet=f.camera.setExtraYaw
f.camera.setExtraYaw=function(v) originalSet(v+0.02) end
f.tick(0,1)
-- Setup applies camera fields but playing readback is the exact commanded frame gate.
if not lastEvent(f,'error') then f.tick(0.1,0.1) end
check(lastEvent(f,'error')~=nil, 'setter readback mismatch is detected rather than accepted as a successful camera command')
f.camera.setExtraYaw=originalSet
for i=1,7 do f.tick(i,1) end
check(lastEvent(f,'finish').reason=='playback_error', 'readback mismatch keeps preview incomplete')
f=playerFixture(0,nil,nil,layeredRoute());ready(f);f.tick(0,1);f.tick(2,2)
local terminalCheck=lastEvent(f,'check')
check(terminalCheck.expected_time==2 and terminalCheck.camera_layer_command_phase=='terminal_reference_endpoint; before_release'
    and terminalCheck.command_camera_layers and terminalCheck.camera_layers_after_write
    and terminalCheck.camera_layer_postwrite_errors and terminalCheck.first_person_offset_postwrite_error==0,
    'successful layered route terminal check retains the commanded additive endpoint and readback proof')
f.policy({session=string.rep('a',32),event='policy_stopped',ready=true,qualified=true,pending_disable=0})
for i=1,4 do f.tick(2+i*0.3,0.3) end
local layeredFinish=lastEvent(f,'finish')
check(f.isQuit() and layeredFinish.reason=='route_finished' and layeredFinish.route_playback_finished
    and layeredFinish.camera_layers_restored and layeredFinish.camera_layer_restoration_failed==false
    and layeredFinish.maximum_camera_layer_postwrite_error==0 and layeredFinish.maximum_first_person_offset_postwrite_error==0,
    'successful layered route exports complete camera diagnostics after terminal check and restoration')
check(layeredFinish.benchmark_qualified==false and layeredFinish.qualification=='PREVIEW_NOT_BENCHMARK_VALIDATED',
    'successful new camera profile remains a preview rather than a benchmark claim')
check(near(f.layers.extra_pitch,0.02) and f.layers.first_person_offset.x==4,
    'successful completion restores camera ownership as well as abort')
f=playerFixture(0,nil,nil,layeredRoute());ready(f);f.tick(0,1)
originalSet=f.camera.setExtraYaw
f.camera.setExtraYaw=function(v) originalSet(v+0.02) end
f.tick(0.1,0.1)
check(lastEvent(f,'error')~=nil, 'every-frame readback catches a mismatch before the next throttled check')
f.camera.setExtraYaw=originalSet
for i=1,7 do f.tick(i,1) end
check(lastEvent(f,'finish').reason=='playback_error', 'between-check mismatch never reports a successful layered route')
local boundaryRoute=layeredRoute()
for _,s in ipairs(boundaryRoute.samples) do s.camera.extra_yaw=math.pi end
f=playerFixture(0,nil,nil,boundaryRoute);ready(f)
originalSet=f.camera.setExtraYaw
f.camera.setExtraYaw=function(v)
    originalSet(v==math.pi and 3.1415927410125732 or v)
end
f.tick(0,1)
check(not lastEvent(f,'error') and lastEvent(f,'check').camera_layers_after_write.extra_yaw>math.pi,
    'actual native float rounding at pi is retained and allowed only in getter readback')
check(lastEvent(f,'check').camera_layer_postwrite_errors.extra_yaw<0.000001,
    'float rounding remains inside the strict command-to-getter mismatch budget')
f.tick(2,2);f.policy({session=string.rep('a',32),event='policy_stopped',ready=true,qualified=true,pending_disable=0})
for i=1,4 do f.tick(2+i*0.3,0.3) end
check(lastEvent(f,'finish').reason=='route_finished' and lastEvent(f,'finish').camera_layers_restored,
    'float-boundary getter readback can finish and restore a complete layered preview')
r=layeredRoute();r.samples[1].camera.extra_yaw=3.1415927410125732
check(not pcall(pure.validate,r), 'readback-only float slack never widens compiled source admission')
r=layeredRoute();r.samples[1].camera.extra_yaw=math.pi+0.0000011
check(not pcall(pure.validateCameraLayers,r.samples[1].camera,true), 'getter slack is bounded rather than a general camera guard bypass')
print('Playback core and PLAYER mock checks passed: '..checks)
