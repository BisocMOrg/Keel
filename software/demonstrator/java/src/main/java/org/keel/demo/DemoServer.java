// SPDX-License-Identifier: MIT
package org.keel.demo;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Base64;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** LAN demonstrator server. Hardware lives behind the existing Java binding. */
public final class DemoServer implements AutoCloseable {
    private final HttpServer server;
    private final ThreadPoolExecutor executor;
    private final RobotControl control;
    private final CameraWorker camera;
    private final Path webRoot;
    private final byte[] bearer;
    private final long started=System.nanoTime();

    DemoServer(String host,int port,String mode,boolean allowMotion,Path webRoot,String token) throws IOException {
        this.webRoot=webRoot.toAbsolutePath().normalize();
        bearer=("Bearer "+token).getBytes(StandardCharsets.UTF_8);
        server=HttpServer.create(new InetSocketAddress(host,port),32);
        RobotControl opened=null;
        CameraWorker openedCamera=null;
        try {
            opened=new RobotControl(mode,allowMotion);
            openedCamera=new CameraWorker(mode);
        } catch(RuntimeException|Error failure) {
            if(openedCamera!=null) openedCamera.close();
            if(opened!=null) opened.close();
            server.stop(0);
            throw failure;
        }
        control=opened;
        camera=openedCamera;
        executor=new ThreadPoolExecutor(4,16,30,TimeUnit.SECONDS,new ArrayBlockingQueue<>(64),
                Thread.ofPlatform().daemon().name("demo-http-",0).factory(),new ThreadPoolExecutor.AbortPolicy());
        server.setExecutor(executor);
        server.createContext("/",this::handle);
    }
    void start() { server.start(); }
    int port() { return server.getAddress().getPort(); }
    private void handle(HttpExchange x) throws IOException {
        try {
            x.getResponseHeaders().set("X-Content-Type-Options","nosniff");
            x.getResponseHeaders().set("Referrer-Policy","no-referrer");
            String path=x.getRequestURI().getPath(),method=x.getRequestMethod();
            if(path.startsWith("/api/")) {
                x.getResponseHeaders().set("Cache-Control","no-store");
                String authorization=x.getRequestHeaders().getFirst("Authorization");
                if(authorization==null || authorization.length()>1024 || !MessageDigest.isEqual(bearer,authorization.getBytes(StandardCharsets.UTF_8)))
                    throw new ApiError(401,"Enter the access token printed by the demo server");
                if(!method.equals("GET")&&!method.equals("POST")) throw new ApiError(405,"Method not allowed");
                if(method.equals("POST")) checkOrigin(x);
                if(path.equals("/api/ping") && method.equals("GET")) {
                    double received=nowMs();json(x,200,Json.map("serverReceiveMs",received,"serverSendMs",nowMs()));return;
                }
                if(path.equals("/api/state") && method.equals("GET")) {
                    json(x,200,Json.map("language","Java","mode",control.mode,"serverTimeMs",nowMs(),
                            "motionAllowed",control.motionAllowed,"maxDuty",RobotControl.MAX_DUTY,"driveTimeoutMs",350,
                            "controller",control.controller(),"robot",control.robotState(),"loop",control.loopState(),
                            "camera",camera.snapshot(),"system",Json.map("uptimeS",(System.nanoTime()-started)/1e9,"cpuTempC",temperature())));return;
                }
                if(path.equals("/api/command") && method.equals("POST")) {
                    String contentType=x.getRequestHeaders().getFirst("Content-Type");
                    if(contentType==null || !contentType.split(";",2)[0].trim().equalsIgnoreCase("application/json"))
                        throw new ApiError(415,"Commands require application/json");
                    byte[] bytes=x.getRequestBody().readNBytes(8193);
                    if(bytes.length>8192) throw new ApiError(413,"Command body too large");
                    String body=StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
                    Map<String,Object> request=Json.object(body);
                    var future=control.command(request,camera);
                    Map<String,Object> result;
                    try { result=future.get(15,TimeUnit.SECONDS); }
                    catch(TimeoutException|InterruptedException e) { future.cancel(false);throw e; }
                    json(x,200,result);return;
                }
                if(path.equals("/api/camera/frame") && method.equals("GET")) {
                    long after=0;String query=x.getRequestURI().getRawQuery();
                    if(query!=null) { if(!query.matches("after=[0-9]{1,18}")) throw new ApiError(400,"Expected after=nonnegative sequence"); after=Long.parseLong(query.substring(6)); }
                    var frame=camera.frame(after);
                    if(frame==null) { x.sendResponseHeaders(204,-1);return; }
                    double sent=nowMs();
                    x.getResponseHeaders().set("X-Sequence",Long.toString(frame.sequence()));
                    x.getResponseHeaders().set("X-Captured-Ms",Double.toString(frame.capturedMs()));
                    x.getResponseHeaders().set("X-Sent-Ms",Double.toString(sent));
                    x.getResponseHeaders().set("X-Timestamp-Basis",frame.basis());
                    x.getResponseHeaders().set("X-Capture-Age-Ms",Double.toString(Math.max(0,sent-frame.capturedMs())));
                    send(x,200,"image/jpeg",frame.jpeg());return;
                }
                if(path.startsWith("/api/photos/") && method.equals("GET")) {
                    String id=path.substring("/api/photos/".length());
                    if(!id.matches("[a-f0-9-]{36}\\.jpg")) throw new ApiError(404,"Photo not found");
                    byte[] bytes=camera.photo(id);if(bytes==null) throw new ApiError(404,"Photo expired or not found");
                    x.getResponseHeaders().set("Content-Disposition","attachment; filename=\"keel-"+id+"\"");
                    send(x,200,"image/jpeg",bytes);return;
                }
                throw new ApiError(404,"API route not found");
            }
            if(!method.equals("GET") && !method.equals("HEAD")) throw new ApiError(405,"Method not allowed");
            if(path.equals("/")) path="/index.html";
            Path file=webRoot.resolve(path.substring(1)).normalize();
            if(!file.startsWith(webRoot) || !Files.isRegularFile(file)) throw new ApiError(404,"File not found; check --web-root");
            if(!file.toRealPath().startsWith(webRoot.toRealPath())) throw new ApiError(403,"Path not allowed");
            String type=contentType(file);
            x.getResponseHeaders().set("Cache-Control",file.getFileName().toString().equals("index.html")?"no-cache":"public, max-age=3600");
            if(method.equals("HEAD")) { x.getResponseHeaders().set("Content-Type",type);x.sendResponseHeaders(200,-1); }
            else {
                x.getResponseHeaders().set("Content-Type",type);x.sendResponseHeaders(200,Files.size(file));
                Files.copy(file,x.getResponseBody());
            }
        } catch(ApiError e) { json(x,e.status,Json.map("error",e.getMessage())); }
        catch(IllegalArgumentException | CharacterCodingException e) { json(x,400,Json.map("error",e.getMessage())); }
        catch(TimeoutException e) { json(x,504,Json.map("error","Operation timed out; check robot and camera status")); }
        catch(ExecutionException e) {
            Throwable cause=e.getCause();while(cause instanceof java.util.concurrent.CompletionException && cause.getCause()!=null) cause=cause.getCause();
            json(x,cause instanceof ApiError a?a.status:409,Json.map("error",cause.getMessage()==null?cause.getClass().getSimpleName():cause.getMessage()));
        } catch(InterruptedException e) { Thread.currentThread().interrupt();json(x,503,Json.map("error","Server shutting down")); }
        catch(Throwable e) {
            if(!(e instanceof IOException)) System.err.println("Request failed: "+e.getClass().getSimpleName()+": "+e.getMessage());
            try { json(x,503,Json.map("error","Request failed; inspect server log")); } catch(IOException ignored) { }
        } finally { x.close(); }
    }
    private static void checkOrigin(HttpExchange x) {
        String origin=x.getRequestHeaders().getFirst("Origin"),host=x.getRequestHeaders().getFirst("Host");
        String site=x.getRequestHeaders().getFirst("Sec-Fetch-Site");
        if("cross-site".equals(site)) throw new ApiError(403,"Cross-site commands are refused");
        if(origin!=null) {
            try {
                URI uri=URI.create(origin);
                if(!"http".equals(uri.getScheme()) || uri.getRawUserInfo()!=null || uri.getHost()==null ||
                        !uri.getRawAuthority().equalsIgnoreCase(host) || (uri.getPath()!=null&&!uri.getPath().isEmpty()) || uri.getQuery()!=null || uri.getFragment()!=null)
                    throw new ApiError(403,"Cross-origin commands are refused");
            } catch(IllegalArgumentException e) { throw new ApiError(403,"Invalid Origin"); }
        }
    }
    private static Double temperature() {
        try { return Double.parseDouble(Files.readString(Path.of("/sys/class/thermal/thermal_zone0/temp")).trim())/1000; }
        catch(Exception e) { return null; }
    }
    private static double nowMs() { return System.nanoTime()/1e6; }
    private static String contentType(Path file) {
        String name=file.getFileName().toString().toLowerCase(java.util.Locale.ROOT);
        if(name.endsWith(".html")) return "text/html; charset=utf-8";
        if(name.endsWith(".js")) return "text/javascript; charset=utf-8";
        if(name.endsWith(".css")) return "text/css; charset=utf-8";
        if(name.endsWith(".json")) return "application/json";
        if(name.endsWith(".glb")) return "model/gltf-binary";
        if(name.endsWith(".svg")) return "image/svg+xml";
        if(name.endsWith(".png")) return "image/png";
        if(name.endsWith(".jpg")||name.endsWith(".jpeg")) return "image/jpeg";
        if(name.endsWith(".webp")) return "image/webp";
        return "application/octet-stream";
    }
    private static void json(HttpExchange x,int status,Object value) throws IOException { send(x,status,"application/json; charset=utf-8",Json.encode(value).getBytes(StandardCharsets.UTF_8)); }
    private static void send(HttpExchange x,int status,String type,byte[] data) throws IOException {
        x.getResponseHeaders().set("Content-Type",type);x.sendResponseHeaders(status,data.length);x.getResponseBody().write(data);
    }
    @Override public void close() {
        try { control.close(); } finally { server.stop(1);executor.shutdownNow();camera.close(); }
    }
    public static void main(String[] args) throws Exception {
        String mode="mock",host="127.0.0.1",token=System.getenv("KEEL_DEMO_TOKEN");int port=8788;
        Path webRoot=Path.of("../web/dist"),tokenFile=null;boolean allowMotion=false;
        for(int i=0;i<args.length;i++) {
            switch(args[i]) {
                case "--mode" -> mode=args[++i]; case "--host" -> host=args[++i];
                case "--port" -> port=Integer.parseInt(args[++i]);case "--web-root" -> webRoot=Path.of(args[++i]);
                case "--token-file" -> tokenFile=Path.of(args[++i]);case "--allow-motion" -> allowMotion=true;
                case "--help" -> { System.out.println("Keel Java demonstrator: --mode mock|simulator|hardware --host HOST --port PORT --web-root PATH --token-file PATH [--allow-motion]");return; }
                default -> throw new IllegalArgumentException("Unknown argument: "+args[i]);
            }
        }
        if(!Set.of("mock","simulator","hardware").contains(mode)) throw new IllegalArgumentException("Unknown mode");
        if(port<0||port>65535) throw new IllegalArgumentException("Invalid port");
        if(tokenFile!=null) token=Files.readString(tokenFile).trim();
        boolean generated=token==null||token.isBlank();
        if(generated) { byte[] random=new byte[24];new SecureRandom().nextBytes(random);token=Base64.getUrlEncoder().withoutPadding().encodeToString(random); }
        if(token.length()<16||token.length()>512||token.chars().anyMatch(Character::isWhitespace)) throw new IllegalArgumentException("Token must be 16..512 non-whitespace characters");
        DemoServer app=new DemoServer(host,port,mode,allowMotion,webRoot,token);
        Runtime.getRuntime().addShutdownHook(Thread.ofPlatform().unstarted(app::close));
        app.start();
        System.out.println("Keel Java demonstrator / "+mode+" at http://"+host+":"+app.port());
        if(generated) System.out.println("Access token: "+token);
        else System.out.println("Access token loaded from configured file/environment.");
        System.out.println("Real motor arming: "+(mode.equals("hardware")&&allowMotion?"enabled":"disabled"));
    }
}
