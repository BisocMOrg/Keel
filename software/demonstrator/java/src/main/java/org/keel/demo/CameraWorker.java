// SPDX-License-Identifier: MIT
package org.keel.demo;

import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.CameraOptions;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.encoding.EncoderOptions;
import io.github.bisocmorg.keel.camera.errors.CameraTimeoutException;
import java.awt.Color;
import java.awt.Font;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.image.BufferedImage;
import java.io.ByteArrayOutputStream;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import javax.imageio.ImageIO;

/** One platform thread owns every native camera operation. Latest-frame storage never blocks capture. */
final class CameraWorker implements AutoCloseable {
    record Frame(long sequence,double capturedMs,String basis,byte[] jpeg) {}
    private record Request(String type,Runnable validate,CompletableFuture<Map<String,Object>> future) {}
    private final boolean mock;
    private final ArrayBlockingQueue<Request> requests=new ArrayBlockingQueue<>(8);
    private final Thread worker;
    private final LinkedHashMap<String,byte[]> photos=new LinkedHashMap<>();
    private long photoBytes;
    private volatile boolean closed,available,autofocus;
    private volatile String state="stopped",error,source;
    private volatile Frame latest;
    private volatile long frames,bytes,dropped;
    private volatile double fps;
    private volatile long lastFrameNs;
    private Camera camera;
    private boolean streaming;
    private long fpsStart,fpsFrames,lastNativeSequence=-1;
    private NativeClock nativeClock;

    CameraWorker(String mode) {
        mock=mode.equals("mock");available=mock;source=mock?"mock — synthetic test image":"libcamera / MJPEG";
        worker=Thread.ofPlatform().daemon().name("demo-camera-owner").start(this::run);
    }
    CompletableFuture<Map<String,Object>> command(String type,Runnable validate) {
        CompletableFuture<Map<String,Object>> future=new CompletableFuture<>();
        if(closed || !requests.offer(new Request(type,validate,future))) future.completeExceptionally(new ApiError(429,"Camera request queue is busy"));
        return future;
    }
    private void run() {
        if(!mock) {
            try {
                var cameras=Camera.listCameras();available=!cameras.isEmpty();
                if(!available) error="No camera detected";
                else { autofocus=cameras.getFirst().autofocus();source=cameras.getFirst().model()+" / MJPEG"; }
            }
            catch(Throwable e) { available=false;error=message(e); }
            try { nativeClock=new NativeClock(); } catch(Throwable e) { nativeClock=null; }
        }
        try {
            while(!closed) {
                Request request=requests.poll(streaming?0:100,TimeUnit.MILLISECONDS);
                if(request!=null) {
                    if(request.future.isCancelled()) continue;
                    try { request.validate.run(); request.future.complete(action(request.type,()->!closed&&!request.future.isCancelled())); }
                    catch(Throwable e) { error=message(e); request.future.completeExceptionally(new ApiError(409,error)); }
                }
                if(streaming && !closed) {
                    try {
                        if(mock) {
                            long started=System.nanoTime();
                            byte[] jpeg=synthetic(640,480,frames+1);
                            publish(jpeg,System.nanoTime()/1e6,"server-receipt");
                            long remaining=50_000_000L-(System.nanoTime()-started);
                            if(remaining>0) TimeUnit.NANOSECONDS.sleep(remaining);
                        } else {
                            var chunk=camera.readChunk(Duration.ofMillis(100));
                            long receipt=System.nanoTime();
                            double captured=receipt/1e6;String basis="server-receipt";
                            if(nativeClock!=null && chunk.timestampNs()>0) {
                                double mapped=nativeClock.map(chunk.timestampNs());
                                double age=receipt/1e6-mapped;
                                if(age>=0 && age<=5000) { captured=mapped;basis="sensor"; }
                            }
                            if(lastNativeSequence>=0 && chunk.sequence()>lastNativeSequence+1) dropped+=chunk.sequence()-lastNativeSequence-1;
                            lastNativeSequence=chunk.sequence();
                            publish(chunk.toByteArray(),captured,basis);
                        }
                    } catch(CameraTimeoutException timeout) {
                        // A stalled camera cannot masquerade as a live, high-FPS feed forever.
                        if(System.nanoTime()-lastFrameNs>3_000_000_000L) {
                            error="Camera delivered no frame for 3 seconds";state="error";
                            streaming=false;latest=null;fps=0;closeCamera();
                        }
                    } catch(Throwable e) {
                        error=message(e);state="error";streaming=false;latest=null;closeCamera();
                    }
                }
            }
        } catch(InterruptedException e) { Thread.currentThread().interrupt(); }
        finally {
            closeCamera();
            Request r;while((r=requests.poll())!=null) r.future.completeExceptionally(new ApiError(503,"Camera stopped"));
            latest=null;state="stopped";
        }
    }
    private void publish(byte[] jpeg,double captured,String basis) {
        frames++;bytes+=jpeg.length;latest=new Frame(frames,captured,basis,jpeg);lastFrameNs=System.nanoTime();
        long now=System.nanoTime();
        if(fpsStart==0) { fpsStart=now;fpsFrames=frames; }
        if(now-fpsStart>=1_000_000_000L) { fps=(frames-fpsFrames)*1e9/(now-fpsStart);fpsStart=now;fpsFrames=frames; }
    }
    private Map<String,Object> action(String type,java.util.function.BooleanSupplier active) throws Exception {
        switch(type) {
            case "cameraStart" -> { if(!streaming) start(); }
            case "cameraStop" -> { stop(); }
            case "autofocus" -> {
                if(mock) throw new ApiError(409,"Mock camera has no autofocus");
                boolean opened=camera==null;
                try { ensureCamera();camera.triggerAutofocus(Duration.ofSeconds(3)); }
                finally { if(opened) closeCamera(); }
            }
            case "photo" -> {
                boolean resume=streaming;
                byte[] jpeg;
                stop();state="capturing";
                try {
                    if(mock) jpeg=synthetic(1280,960,frames);
                    else {
                        // A separate stopped session uses the sensor's full still resolution.
                        try(Camera still=Camera.open()) { jpeg=still.captureStill().toByteArray(); }
                    }
                } finally {
                    state="stopped";
                    if(resume && active.getAsBoolean()) start();
                }
                if(!active.getAsBoolean()) throw new ApiError(409,"Photo request cancelled");
                String id=UUID.randomUUID()+".jpg";
                synchronized(photos) {
                    if(jpeg.length>24*1024*1024) throw new ApiError(413,"Photo exceeds demo capture limit");
                    while(!photos.isEmpty() && (photos.size()>=8 || photoBytes+jpeg.length>48*1024*1024)) {
                        String oldest=photos.keySet().iterator().next();photoBytes-=photos.remove(oldest).length;
                    }
                    photos.put(id,jpeg);photoBytes+=jpeg.length;
                }
                error=null;
                return Json.map("ok",true,"photo","/api/photos/"+id);
            }
            default -> throw new IllegalArgumentException("Unknown camera command");
        }
        error=null;return Json.map("ok",true);
    }
    private void ensureCamera() {
        if(camera==null) camera=Camera.open(CameraOptions.builder().width(640).height(480).framerate(20).frameQueue(1).build());
        available=true;
    }
    private void start() {
        if(!mock) {
            try { ensureCamera();camera.setEncoder(EncoderOptions.builder(Codec.MJPEG).quality(80).encodedQueue(2).build());camera.start(); }
            catch(Throwable e) { closeCamera();state="error";throw e; }
        }
        streaming=true;state="streaming";latest=null;lastNativeSequence=-1;fpsStart=0;fps=0;lastFrameNs=System.nanoTime();
    }
    private void stop() { streaming=false;latest=null;fps=0;closeCamera();state="stopped"; }
    private void closeCamera() { if(camera!=null) { try { camera.close(); } finally { camera=null; } } }
    Frame frame(long after) { Frame f=latest;return f!=null&&f.sequence>after?f:null; }
    byte[] photo(String id) { synchronized(photos) { return photos.get(id); } }
    Map<String,Object> snapshot() {
        double liveFps=System.nanoTime()-lastFrameNs>1_000_000_000L?0:fps;
        return Json.map("available",available,"state",state,"width",640,"height",480,"fps",liveFps,
                "frames",frames,"dropped",dropped,"bytes",bytes,"source",source,"error",error,"autofocus",autofocus);
    }
    static byte[] synthetic(int width,int height,long sequence) throws Exception {
        BufferedImage image=new BufferedImage(width,height,BufferedImage.TYPE_INT_RGB);
        Graphics2D g=image.createGraphics();
        try {
            g.setRenderingHint(RenderingHints.KEY_ANTIALIASING,RenderingHints.VALUE_ANTIALIAS_ON);
            g.setColor(new Color(12,23,34));g.fillRect(0,0,width,height);
            g.setColor(new Color(28,46,60));for(int x=0;x<width;x+=40)g.drawLine(x,0,x,height);for(int y=0;y<height;y+=40)g.drawLine(0,y,width,y);
            int x=(int)((sequence*7)%(width-120));g.setColor(new Color(47,210,175));g.fillRoundRect(x,height/2,100,70,18,18);
            g.setColor(Color.WHITE);g.setFont(new Font(Font.SANS_SERIF,Font.BOLD,width/28));g.drawString("KEEL / SYNTHETIC CAMERA",28,55);
            g.setFont(new Font(Font.MONOSPACED,Font.PLAIN,width/40));g.drawString("JAVA MOCK — NO HARDWARE",28,90);
            g.drawString("Frame "+sequence+"   "+width+" x "+height,28,height-55);
            g.drawString("Server clock "+String.format(java.util.Locale.ROOT,"%.0f",System.nanoTime()/1e6)+" ms",28,height-25);
        } finally { g.dispose(); }
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        if(!ImageIO.write(image,"JPEG",out)) throw new IllegalStateException("JPEG encoder unavailable");
        return out.toByteArray();
    }
    private static String message(Throwable e) { return e.getMessage()==null?e.getClass().getSimpleName():e.getMessage(); }
    @Override public void close() {
        closed=true;worker.interrupt();
        try { worker.join(5000); } catch(InterruptedException e) { Thread.currentThread().interrupt(); }
    }
    /** Explicit FFM clock bridge: never assume System.nanoTime shares libc's epoch. */
    private static final class NativeClock {
        private final double offsetNs;
        NativeClock() throws Throwable {
            Linker linker=Linker.nativeLinker();
            MethodHandle call=linker.downcallHandle(linker.defaultLookup().find("clock_gettime").orElseThrow(),
                    FunctionDescriptor.of(ValueLayout.JAVA_INT,ValueLayout.JAVA_INT,ValueLayout.ADDRESS));
            long best=Long.MAX_VALUE;double offset=0;
            try(Arena arena=Arena.ofConfined()) {
                MemorySegment ts=arena.allocate(16,8);
                for(int i=0;i<12;i++) {
                    long before=System.nanoTime();int rc=(int)call.invokeExact(1,ts);long after=System.nanoTime();
                    if(rc!=0) throw new IllegalStateException("clock_gettime failed");
                    long nativeNs=ts.get(ValueLayout.JAVA_LONG,0)*1_000_000_000L+ts.get(ValueLayout.JAVA_LONG,8);
                    if(after-before<best) { best=after-before;offset=before+(after-before)/2.0-nativeNs; }
                }
            }
            offsetNs=offset;
        }
        double map(long nativeNs) { return (nativeNs+offsetNs)/1e6; }
    }
}
