// SPDX-License-Identifier: MIT
package org.keel.demo;

import io.github.bisocmorg.keel.Backend;
import io.github.bisocmorg.keel.Capability;
import io.github.bisocmorg.keel.OpenOptions;
import io.github.bisocmorg.keel.errors.HandleException;
import io.github.bisocmorg.keel.task.RobotSession;
import io.github.bisocmorg.keel.task.TaskException;
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
import java.util.function.Function;
import java.util.concurrent.ExecutorService;

/** Single controller and application-level deadman, independent of HTTP and camera work. */
final class RobotControl implements AutoCloseable {
    static final int MAX_DUTY=350;
    static final long DRIVE_NS=350_000_000L, OWNER_NS=1_500_000_000L;
    final String mode;
    final boolean motionAllowed;
    private final LongSupplier clock;
    private final RobotSession session;
    private final ExecutorService rangeWorker=Executors.newSingleThreadExecutor(Thread.ofPlatform().daemon().name("demo-range").factory());
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
    private boolean closed;
    private long cycles,lastTick,maxGap,overruns;
    private int pendingNative;

    /** Opens the chosen backend and its application ownership watchdog. */
    RobotControl(String mode,boolean allowMotion) {
        this(mode,allowMotion,System::nanoTime,true);
    }
    /** Constructs an optionally manual-clock mock for protocol tests, or a managed native session. */
    RobotControl(String mode,boolean allowMotion,LongSupplier clock,boolean startMockTimer) {
        this.mode=mode; this.clock=clock;
        motionAllowed=!mode.equals("hardware")||allowMotion;
        if(mode.equals("mock")) {
            session=null;
            capabilities=Set.of(Capability.MOTORS,Capability.RGB,Capability.BUTTONS,Capability.BUTTON_LEDS,Capability.RANGE,Capability.SIMULATED);
            watchdog=startMockTimer?Executors.newSingleThreadScheduledExecutor(Thread.ofPlatform().daemon().name("demo-deadman").factory()):null;
            if(watchdog!=null) watchdog.scheduleAtFixedRate(this::tick,0,10,TimeUnit.MILLISECONDS);
        } else {
            session=RobotSession.open(OpenOptions.builder().backend(mode.equals("simulator")?Backend.SIMULATOR:Backend.R3).build());
            capabilities=session.capabilities();
            watchdog=Executors.newSingleThreadScheduledExecutor(Thread.ofPlatform().daemon().name("demo-deadman").factory());
            watchdog.scheduleAtFixedRate(this::tick,0,10,TimeUnit.MILLISECONDS);
        }
    }
    /** Expires browser ownership and input leases; the session owns all hardware ticking. */
    synchronized void tick() {
        if(closed) return;
        long now=clock.getAsLong();
        if(lastTick!=0) { long gap=now-lastTick; maxGap=Math.max(maxGap,gap); if(gap>20_000_000) overruns++; }
        lastTick=now; cycles++;
        expire(now);
    }
    /** Invalidates stale browser authority without renewing any received drive deadline. */
    private void expire(long now) {
        boolean ownerExpired=owner!=null && now>=ownerUntil;
        if(ownerExpired || (driveUntil!=0 && now>=driveUntil)) {
            halt(false);
            if(ownerExpired) { owner=null; ownerUntil=0; }
        }
    }
    /** Revokes queued commands before applying a stop to the selected backend. */
    private void halt(boolean emergency) {
        left=right=0; driveUntil=0; generation++;
        if(session!=null) {
            try { if(emergency) session.emergencyStop(); else session.disarm(); }
            catch(HandleException e) { if(!closed) throw e; }
        }
        else if(emergency) { mockState="FAULTED"; fault="Emergency stop"; }
        else if(!mockState.equals("FAULTED")) mockState="DISARMED";
    }
    /** Rejects a caller without the current unexpired browser-control lease. */
    private void requireOwner(String id,long now) {
        if(!id.equals(owner)||now>=ownerUntil) throw new ApiError(409,"Claim control first; controller lease expired or belongs to another browser");
    }
    /** Rejects an operation unsupported by the selected board. */
    private void requireCapability(Capability cap) {
        if(!capabilities.contains(cap)) throw new ApiError(409,"This board does not provide "+cap);
    }
    /** Revalidates owner, original request expiry and generation before native execution. */
    private CompletableFuture<Map<String,Object>> nativeAction(String id,long ticket,
            Function<Runnable,CompletableFuture<Void>> operation,Runnable after) {
        if(session==null) { after.run(); return done(); }
        if(!session.running()) throw new ApiError(503,"Board session is not running");
        if(pendingNative>=4) throw new ApiError(429,"Board command queue is busy");
        pendingNative++;
        long requestExpires=clock.getAsLong()+15_000_000_000L;
        CompletableFuture<Map<String,Object>> result=new CompletableFuture<>();
        Runnable guard=()->{ synchronized(this) {
            long now=clock.getAsLong(); expire(now); requireOwner(id,now);
            if(result.isCancelled() || now>=requestExpires || ticket!=generation)
                throw new ApiError(409,"Command cancelled or expired before execution");
        }};
        try {
            CompletableFuture<Void> nativeResult=operation.apply(guard);
            result.whenComplete((value,error)->{
                if(result.isCancelled()) {
                    nativeResult.cancel(false);
                    synchronized(this) { halt(false); }
                }
            });
            nativeResult.whenComplete((value,error)->{
                synchronized(this) { pendingNative--; }
                if(error!=null) result.completeExceptionally(error);
                else { synchronized(this) { after.run(); } result.complete(Json.map("ok",true)); }
            });
            return result;
        } catch(RuntimeException failure) { pendingNative--; throw failure; }
    }
    /** Validates one browser request and dispatches a bounded managed operation. */
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
                    return nativeAction(id,ticket,guard->session.requestArm(guard),()->{
                        if(session==null) {
                            if(mockState.equals("FAULTED")) throw new ApiError(409,"Clear the fault before arming");
                            mockState="ARMED";
                        }
                    });
                }
                case "clearFault" -> { halt(false);
                    return nativeAction(id,generation,guard->session.clearFaultAsync(guard),()->{ if(session==null) { mockState="DISARMED"; fault=null; }}); }
                case "drive" -> {
                    int l=(int)Json.integer(request,"left",-MAX_DUTY,MAX_DUTY);
                    int r=(int)Json.integer(request,"right",-MAX_DUTY,MAX_DUTY);
                    requireCapability(Capability.MOTORS);
                    if(!motionAllowed) throw new ApiError(403,"Real motion requires --allow-motion");
                    if(session==null && !mockState.equals("ARMED")) throw new ApiError(409,"Robot is not armed");
                    if(session!=null) {
                        try { session.refreshDriveUntil(l/10.0,r/10.0,now+DRIVE_NS); }
                        catch(TaskException refused) {
                            int status=refused.reason()==TaskException.Reason.CANCELLED || refused.reason()==TaskException.Reason.FAULT?409:503;
                            throw new ApiError(status,refused.getMessage());
                        }
                    }
                    left=l;right=r;driveUntil=(l==0&&r==0)?0:now+DRIVE_NS;
                    return done();
                }
                case "rgb" -> {
                    requireCapability(Capability.RGB);
                    int index=(int)Json.integer(request,"index",-1,5),color=(int)Json.integer(request,"color",0,0xFFFFFF);
                    return nativeAction(id,ticket,guard->session.setRgbAsync(index,color,guard),()->{
                        if(index<0) Arrays.fill(rgb,color); else rgb[index]=color;
                    });
                }
                case "buttonLeds" -> {
                    requireCapability(Capability.BUTTON_LEDS);
                    int mask=(int)Json.integer(request,"mask",0,15);
                    return nativeAction(id,ticket,guard->session.setButtonLedsAsync(15,mask,guard),()->buttonLeds=mask);
                }
                case "range" -> {
                    requireCapability(Capability.RANGE);
                    return nativeAction(id,ticket,guard->CompletableFuture.runAsync(()->{
                        guard.run(); var result=session.measureDistance(); guard.run();
                        synchronized(this) { distance=result.valid()?result.distanceMm():null; }
                    },rangeWorker),()->{ if(session==null) distance=420L; });
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
    /** Creates an already-completed protocol acknowledgement. */
    private static CompletableFuture<Map<String,Object>> done() { return CompletableFuture.completedFuture(Json.map("ok",true)); }
    /** Returns browser ownership metadata without exposing its access token. */
    synchronized Map<String,Object> controller() {
        return Json.map("clientId",owner,"expiresInMs",owner==null?0:Math.max(0,(ownerUntil-clock.getAsLong())/1_000_000));
    }
    /** Projects real sensor/output state, or explicitly synthetic mock state, for the dashboard. */
    synchronized Map<String,Object> robotState() {
        int appliedLeft=left,appliedRight=right,buttons=0,lit=buttonLeds;
        String state=mockState,currentFault=fault;
        boolean enabled=state.equals("ARMED");
        if(session!=null) {
            var s=session.snapshot(); state=s.state().name();appliedLeft=s.outputLeft(); appliedRight=s.outputRight();
            buttons=s.buttonsHeldValue();lit=s.buttonLedsValue();
            currentFault=state.equals("FAULTED")?session.fault().cause().name()+": "+session.fault().status().name():null;
            enabled=s.inputs().contains(SafetyInput.ARMED);
        }
        if(!enabled) appliedLeft=appliedRight=0;
        return Json.map("state",state,"left",appliedLeft,"right",appliedRight,"motorEnabled",enabled,
                "rgb",rgb.clone(),"buttonLeds",lit,"buttons",buttons,"distanceMm",distance,
                "fault",currentFault,"capabilities",capabilities.stream().map(Enum::name).sorted().toList());
    }
    /** Projects managed controller timing statistics for the dashboard. */
    synchronized Map<String,Object> loopState() {
        if(session==null) return Json.map("running",!closed,"cycles",cycles,"overruns",overruns,"maxGapMs",maxGap/1e6,"maxStepMs",0);
        var s=session.stats();return Json.map("running",session.running(),"cycles",s.cycles(),"overruns",s.overruns(),
                "maxGapMs",s.maxGapUs()/1000.0,"maxStepMs",s.maxStepUs()/1000.0);
    }
    /** Closes every owned resource even when a native stop reports a failure. */
    @Override public void close() {
        Throwable failure=null;
        synchronized(this) {
            if(closed) return;
            closed=true;
            try { halt(false); } catch(RuntimeException | Error error) { failure=error; }
        }
        try { if(watchdog!=null) watchdog.shutdownNow(); }
        catch(RuntimeException | Error error) { if(failure==null) failure=error; else failure.addSuppressed(error); }
        try { rangeWorker.shutdownNow(); }
        catch(RuntimeException | Error error) { if(failure==null) failure=error; else failure.addSuppressed(error); }
        try { if(session!=null) session.close(); }
        catch(RuntimeException | Error error) { if(failure==null) failure=error; else failure.addSuppressed(error); }
        if(failure instanceof RuntimeException runtime) throw runtime;
        if(failure instanceof Error error) throw error;
    }
}
