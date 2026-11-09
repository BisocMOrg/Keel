// SPDX-License-Identifier: MIT
package org.keel.demo;

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.Device;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.errors.HandleException;
import io.github.bisocmorg.keel.loop.ControlLoop;
import io.github.bisocmorg.keel.loop.Cycle;
import io.github.bisocmorg.keel.peripherals.RangeStatus;
import io.github.bisocmorg.keel.state.SafetyInput;
import io.github.bisocmorg.keel.state.State;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.LongSupplier;

/** Single controller and application-level deadman, independent of HTTP and camera work. */
final class RobotControl implements AutoCloseable {
    static final int MAX_DUTY=350;
    static final long DRIVE_NS=350_000_000L, OWNER_NS=1_500_000_000L;
    final String mode;
    final boolean motionAllowed;
    private final LongSupplier clock;
    private final Device device;
    private final ControlLoop loop;
    private final ScheduledExecutorService watchdog;
    private final Set<Capability> capabilities;
    private final Map<String,Long> sequences=new LinkedHashMap<>();
    private String owner;
    private long ownerUntil,driveUntil,generation;
    private int left,right;
    private final int[] rgb=new int[6];
    private int buttonLeds;
    private String mockState="DISARMED",fault;
    private Long distance;
    private boolean rangePending,closed;
    private long cycles,lastTick,maxGap,overruns;
    private int pendingNative;

    RobotControl(String mode,boolean allowMotion) {
        this(mode,allowMotion,System::nanoTime,true);
    }
    RobotControl(String mode,boolean allowMotion,LongSupplier clock,boolean startMockTimer) {
        this.mode=mode; this.clock=clock;
        motionAllowed=!mode.equals("hardware")||allowMotion;
        if(mode.equals("mock")) {
            device=null; loop=null;
            capabilities=Set.of(Capability.MOTORS,Capability.RGB,Capability.BUTTONS,Capability.BUTTON_LEDS,Capability.RANGE,Capability.SIMULATED);
            watchdog=startMockTimer?Executors.newSingleThreadScheduledExecutor(Thread.ofPlatform().daemon().name("demo-deadman").factory()):null;
            if(watchdog!=null) watchdog.scheduleAtFixedRate(this::tick,0,10,TimeUnit.MILLISECONDS);
        } else {
            device=Keel.open(OpenOptions.builder().backend(mode.equals("simulator")?Backend.SIMULATOR:Backend.R3).build());
            capabilities=device.capabilities();
            loop=ControlLoop.builder(device,this::nativeStep).build();
            watchdog=null;
            loop.start();
        }
    }
    synchronized void tick() {
        if(closed) return;
        long now=clock.getAsLong();
        if(lastTick!=0) { long gap=now-lastTick; maxGap=Math.max(maxGap,gap); if(gap>20_000_000) overruns++; }
        lastTick=now; cycles++;
        expire(now);
    }
    private synchronized void nativeStep(Cycle cycle) {
        if(closed) { cycle.coast(); return; }
        expire(clock.getAsLong());
        if(cycle.snapshot().state()==State.FAULTED) { left=right=0; driveUntil=0; }
        if(cycle.snapshot().state()==State.ARMED && driveUntil>clock.getAsLong()) cycle.drive(left,right);
        else if(cycle.snapshot().state()==State.ARMED) cycle.coast();
        if(rangePending) {
            var r=device.rangePoll();
            if(r.status()!=RangeStatus.PENDING) { distance=r.valid()?r.distanceMm():null; rangePending=false; }
        }
    }
    private void expire(long now) {
        boolean ownerExpired=owner!=null && now>=ownerUntil;
        if(ownerExpired || (driveUntil!=0 && now>=driveUntil)) {
            halt(false);
            if(ownerExpired) { owner=null; ownerUntil=0; }
        }
    }
    private void halt(boolean emergency) {
        left=right=0; driveUntil=0; generation++;
        if(device!=null) {
            // libkeel also installs a JVM shutdown hook. It may have closed the handle first.
            if(!device.isClosed()) {
                try { if(emergency) device.emergencyStop(); else device.disarm(); }
                catch(HandleException e) { if(!device.isClosed()) throw e; }
            }
        }
        else if(emergency) { mockState="FAULTED"; fault="Emergency stop"; }
        else if(!mockState.equals("FAULTED")) mockState="DISARMED";
    }
    private void requireOwner(String id,long now) {
        if(!id.equals(owner)||now>=ownerUntil) throw new ApiError(409,"Claim control first; controller lease expired or belongs to another browser");
    }
    private void requireCapability(Capability cap) {
        if(!capabilities.contains(cap)) throw new ApiError(409,"This board does not provide "+cap);
    }
    private CompletableFuture<Map<String,Object>> nativeAction(String id,long ticket,Runnable action) {
        if(loop==null) { action.run(); return CompletableFuture.completedFuture(Json.map("ok",true)); }
        if(!loop.isRunning()) throw new ApiError(503,"Board control loop is not running");
        if(pendingNative>=4) throw new ApiError(429,"Board command queue is busy");
        CompletableFuture<Map<String,Object>> result=new CompletableFuture<>();
        pendingNative++;
        try {
            loop.submit(d->{ synchronized(this) {
                if(result.isCancelled()) throw new ApiError(409,"Command was cancelled");
                long now=clock.getAsLong(); expire(now); requireOwner(id,now);
                if(ticket!=generation) throw new ApiError(409,"Command cancelled by a newer stop");
                action.run(); return Json.map("ok",true);
            }}).whenComplete((value,error)->{
                synchronized(this) { pendingNative--; }
                if(error!=null) result.completeExceptionally(error);else result.complete(value);
            });
        } catch(RuntimeException e) { pendingNative--;throw e; }
        return result;
    }
    CompletableFuture<Map<String,Object>> command(Map<String,Object> request,CameraWorker camera) {
        String type=Json.text(request,"type"),id=Json.text(request,"clientId");
        if(!id.matches("[A-Za-z0-9_.:-]{1,96}")) throw new IllegalArgumentException("Invalid clientId");
        long seq=Json.integer(request,"seq",1,9_007_199_254_740_991L);
        long ticket;
        synchronized(this) {
            if(closed) throw new ApiError(503,"Server is shutting down");
            long now=clock.getAsLong(); expire(now);
            // Stop is monotonic toward safety: a heartbeat racing ahead of it must not reject it.
            if(Set.of("stop","disarm","estop").contains(type)) {
                if(sequences.containsKey(id)||sequences.size()<128)
                    sequences.put(id,Math.max(seq,sequences.getOrDefault(id,0L)));
                halt(type.equals("estop"));
                if(owner!=null && !owner.equals(id)) { owner=null;ownerUntil=0; }
                return done();
            }
            if(type.equals("release") && id.equals(owner)) {
                sequences.put(id,Math.max(seq,sequences.getOrDefault(id,0L)));
                halt(false);owner=null;ownerUntil=0;return done();
            }
            if(seq<=sequences.getOrDefault(id,0L)) throw new ApiError(409,"Stale command sequence rejected");
            if(!sequences.containsKey(id)&&sequences.size()>=128) throw new ApiError(429,"Too many client identities; restart the demo server");
            sequences.put(id,seq);
            switch(type) {
                case "claim" -> {
                    if(owner!=null&&!owner.equals(id)) throw new ApiError(409,"Another browser owns the controls");
                    owner=id; ownerUntil=now+OWNER_NS;
                    return done();
                }
                default -> requireOwner(id,now);
            }
            ticket=generation;
            switch(type) {
                case "heartbeat" -> { ownerUntil=now+OWNER_NS; return done(); }
                case "release" -> { halt(false); owner=null; ownerUntil=0; return done(); }
                case "arm" -> {
                    requireCapability(Capability.MOTORS);
                    if(!motionAllowed) throw new ApiError(403,"Real motor arming requires server --allow-motion");
                    return nativeAction(id,ticket,()->{ if(device!=null) device.requestArm(); else {
                        if(mockState.equals("FAULTED")) throw new ApiError(409,"Clear the fault before arming");
                        mockState="ARMED";
                    }});
                }
                case "clearFault" -> { halt(false);
                    return nativeAction(id,generation,()->{ if(device!=null) device.clearFault(); else { mockState="DISARMED"; fault=null; }}); }
                case "drive" -> {
                    int l=(int)Json.integer(request,"left",-MAX_DUTY,MAX_DUTY);
                    int r=(int)Json.integer(request,"right",-MAX_DUTY,MAX_DUTY);
                    requireCapability(Capability.MOTORS);
                    if(!motionAllowed) throw new ApiError(403,"Real motion requires --allow-motion");
                    if(!stateName().equals("ARMED")) throw new ApiError(409,"Robot is not armed");
                    left=l;right=r;driveUntil=(l==0&&r==0)?0:now+DRIVE_NS;
                    return done();
                }
                case "rgb" -> {
                    requireCapability(Capability.RGB);
                    int index=(int)Json.integer(request,"index",-1,5),color=(int)Json.integer(request,"color",0,0xFFFFFF);
                    return nativeAction(id,ticket,()->{
                        if(device!=null) { if(index<0) device.setRgbAll(color); else device.setRgb(index,color); }
                        if(index<0) Arrays.fill(rgb,color); else rgb[index]=color;
                    });
                }
                case "buttonLeds" -> {
                    requireCapability(Capability.BUTTON_LEDS);
                    int mask=(int)Json.integer(request,"mask",0,15);
                    return nativeAction(id,ticket,()->{ if(device!=null) device.setButtonLeds(15,mask); buttonLeds=mask; });
                }
                case "range" -> {
                    requireCapability(Capability.RANGE);
                    return nativeAction(id,ticket,()->{ if(device!=null) { device.rangeStart();rangePending=true;distance=null; } else distance=420L; });
                }
                case "cameraStart","cameraStop","photo","autofocus" -> { /* owner-thread camera dispatch below */ }
                default -> throw new IllegalArgumentException("Unknown command: "+type);
            }
        }
        return camera.command(type,()->{
            synchronized(this) { long now=clock.getAsLong();expire(now);requireOwner(id,now);
                if(ticket!=generation) throw new ApiError(409,"Camera request cancelled by stop"); }
        });
    }
    private static CompletableFuture<Map<String,Object>> done() { return CompletableFuture.completedFuture(Json.map("ok",true)); }
    private String stateName() { return device==null?mockState:loop.snapshot().state().name(); }
    synchronized Map<String,Object> controller() {
        return Json.map("clientId",owner,"expiresInMs",owner==null?0:Math.max(0,(ownerUntil-clock.getAsLong())/1_000_000));
    }
    synchronized Map<String,Object> robotState() {
        int appliedLeft=left,appliedRight=right,buttons=0,lit=buttonLeds;
        String state=mockState,currentFault=fault;
        boolean enabled=state.equals("ARMED");
        if(device!=null) {
            var s=loop.snapshot(); state=s.state().name();appliedLeft=s.outputLeft(); appliedRight=s.outputRight();
            buttons=s.buttonsHeldValue();lit=s.buttonLedsValue();
            currentFault=loop.fault().map(f->f.cause().name()+": "+f.status().name()).orElse(null);
            if(!state.equals("FAULTED")) currentFault=null;
            if(loop.error().isPresent()) currentFault=loop.error().get().getMessage();
            enabled=s.inputs().contains(SafetyInput.ARMED);
        }
        if(!enabled) appliedLeft=appliedRight=0;
        return Json.map("state",state,"left",appliedLeft,"right",appliedRight,"motorEnabled",enabled,
                "rgb",rgb.clone(),"buttonLeds",lit,"buttons",buttons,"distanceMm",distance,
                "fault",currentFault,"capabilities",capabilities.stream().map(Enum::name).sorted().toList());
    }
    synchronized Map<String,Object> loopState() {
        if(loop==null) return Json.map("running",!closed,"cycles",cycles,"overruns",overruns,"maxGapMs",maxGap/1e6,"maxStepMs",0);
        var s=loop.stats();return Json.map("running",loop.isRunning(),"cycles",s.cycles(),"overruns",s.overruns(),
                "maxGapMs",s.maxGapUs()/1000.0,"maxStepMs",s.maxStepUs()/1000.0);
    }
    @Override public void close() {
        synchronized(this) { if(closed) return;closed=true;halt(false); }
        if(watchdog!=null) watchdog.shutdownNow();
        try { if(loop!=null) loop.close(); } finally { if(device!=null) device.close(); }
    }
}
