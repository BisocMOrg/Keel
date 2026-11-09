// SPDX-License-Identifier: MIT
package org.keel.demo;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;
import javax.imageio.ImageIO;
import java.io.ByteArrayInputStream;

/** Contract and safety tests run on a clearly marked mock, never real hardware. */
public final class DemoTests {
    private static int checks;
    private static void check(boolean condition,String message) {
        checks++;if(!condition)throw new AssertionError(message);
    }
    private static void rejects(Runnable code,String message) {
        try { code.run();throw new AssertionError(message); } catch(IllegalArgumentException|ApiError expected) { checks++; }
    }
    private static Map<String,Object> cmd(String type,String client,long seq,Object... extra) {
        var map=Json.map("type",type,"clientId",client,"seq",seq);
        for(int i=0;i<extra.length;i+=2) map.put((String)extra[i],extra[i+1]);return map;
    }
    public static void main(String[] args) throws Exception {
        json();deadman();http();
        System.out.println("Java demonstrator: "+checks+" checks passed");
    }
    private static void json() {
        var parsed=Json.object("{\"x\":[1,true,null,\"a\\n\\u0042\"],\"a\":-2.5e1}");
        check(Json.encode(parsed).contains("a\\nB"),"JSON strings round trip");
        rejects(()->Json.object("{\"x\":1,\"x\":2}"),"Duplicate keys must be rejected");
        rejects(()->Json.object("{\"x\":NaN}"),"NaN must be rejected");
        rejects(()->Json.object("{\"x\":01}"),"Leading zero must be rejected");
        rejects(()->Json.object("{\"x\":1} trailing"),"Trailing input must be rejected");
        rejects(()->Json.integer(Json.object("{\"x\":1.5}"),"x",0,4),"Fractional duty must be rejected");
        rejects(()->Json.integer(Json.object("{\"x\":true}"),"x",0,4),"Boolean duty must be rejected");
        rejects(()->Json.integer(Json.object("{\"x\":1e100}"),"x",0,4),"Overflow must be rejected");
    }
    private static void deadman() {
        AtomicLong now=new AtomicLong(1_000_000_000L);
        try(RobotControl robot=new RobotControl("mock",true,now::get,false);CameraWorker camera=new CameraWorker("mock")) {
            robot.command(cmd("claim","a",1),camera).join();
            rejects(()->robot.command(cmd("claim","b",1),camera),"Controller collision");
            robot.command(cmd("arm","a",2),camera).join();
            robot.command(cmd("drive","a",3,"left",200,"right",-200),camera).join();
            check(robot.robotState().get("left").equals(200),"Drive applied");
            now.addAndGet(200_000_000);robot.command(cmd("heartbeat","a",4),camera).join();
            now.addAndGet(151_000_000);robot.tick();
            check(robot.robotState().get("state").equals("DISARMED"),"Heartbeat must not renew drive deadline");
            check(robot.robotState().get("left").equals(0),"Expired drive must coast");
            robot.command(cmd("arm","a",5),camera).join();
            robot.command(cmd("drive","a",6,"left",200,"right",200),camera).join();
            robot.command(cmd("stop","a",8),camera).join();
            rejects(()->robot.command(cmd("drive","a",7,"left",200,"right",200),camera),"Old in-flight drive must not revive stopped motor");
            robot.command(cmd("arm","a",9),camera).join();
            now.addAndGet(1_600_000_000L);robot.tick();
            check(robot.robotState().get("state").equals("DISARMED"),"Controller expiry disarms even without drive");
            check(robot.controller().get("clientId")==null,"Expired owner released");
            robot.command(cmd("claim","b",2),camera).join();
            robot.command(cmd("estop","a",10),camera).join();
            check(robot.robotState().get("state").equals("FAULTED"),"Non-owner can emergency stop");
            check(robot.controller().get("clientId")==null,"Non-owner stop revokes old controller");
            rejects(()->robot.command(cmd("arm","b",3),camera),"Old owner cannot send delayed arm after foreign stop");
            robot.command(cmd("claim","b",4),camera).join();
            robot.command(cmd("clearFault","b",5),camera).join();
            check(robot.robotState().get("state").equals("DISARMED"),"Fault clear never rearms");
            rejects(()->robot.command(cmd("drive","b",6,"left",351,"right",0),camera),"Duty limit");
            robot.command(cmd("rgb","b",7,"index",-1,"color",0x12FF34),camera).join();
            int[] colors=(int[])robot.robotState().get("rgb");check(colors.length==6&&colors[5]==0x12FF34,"Six LED frame");
            robot.command(cmd("heartbeat","b",9),camera).join();
            robot.command(cmd("estop","b",8),camera).join();
            check(robot.robotState().get("state").equals("FAULTED"),"A newer heartbeat must never prevent emergency stop");
            rejects(()->robot.command(cmd("arm","b",8),camera),"Safety exception does not relax stale arm rejection");
            robot.command(cmd("release","b",8),camera).join();
            check(robot.controller().get("clientId")==null,"Stale release by current owner must still revoke control");
        }
    }
    private static void http() throws Exception {
        Path root=Files.createTempDirectory("keel-demo-java-test-");
        Files.writeString(root.resolve("index.html"),"<html>Keel test</html>");
        String token="test-token-long-enough-for-demo";
        try(DemoServer server=new DemoServer("127.0.0.1",0,"mock",false,root,token)) {
            server.start();String base="http://127.0.0.1:"+server.port();
            HttpClient http=HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(2)).build();
            check(send(http,base,"GET","/",null,null,null).statusCode()==200,"Serve UI");
            check(send(http,base,"GET","/api/state",null,null,null).statusCode()==401,"Authentication required");
            var state=send(http,base,"GET","/api/state",token,null,null);
            check(state.statusCode()==200&&new String(state.body()).contains("\"language\":\"Java\""),"State language");
            check(send(http,base,"POST","/api/command",token,Json.encode(cmd("claim","test",1)),"http://evil.invalid").statusCode()==403,"Cross-origin commands rejected");
            check(send(http,base,"POST","/api/command",token,"x".repeat(8193),null).statusCode()==413,"Oversized body rejected");
            check(send(http,base,"POST","/api/command",token,Json.encode(cmd("claim","test",1)),base).statusCode()==200,"Claim with same origin");
            check(send(http,base,"POST","/api/command",token,Json.encode(cmd("cameraStart","test",2)),base).statusCode()==200,"Start camera");
            var frame=send(http,base,"GET","/api/camera/frame?after=0",token,null,null);
            long deadline=System.nanoTime()+3_000_000_000L;
            while(frame.statusCode()==204 && System.nanoTime()<deadline) {
                Thread.sleep(50);frame=send(http,base,"GET","/api/camera/frame?after=0",token,null,null);
            }
            check(frame.statusCode()==200,"Frame received");
            check(frame.headers().firstValue("X-Timestamp-Basis").orElse("").equals("server-receipt"),"Mock timestamp labeled honestly");
            var image=ImageIO.read(new ByteArrayInputStream(frame.body()));check(image.getWidth()==640&&image.getHeight()==480,"Valid JPEG stream frame");
            check(send(http,base,"GET","/api/camera/frame?after=999999",token,null,null).statusCode()==204,"No stale frame replay");
            send(http,base,"POST","/api/command",token,Json.encode(cmd("claim","test",3)),base);
            var photo=send(http,base,"POST","/api/command",token,Json.encode(cmd("photo","test",4)),base);
            String path=(String)Json.object(new String(photo.body())).get("photo");
            var jpeg=send(http,base,"GET",path,token,null,null);check(jpeg.statusCode()==200,"Photo download");
            check(ImageIO.read(new ByteArrayInputStream(jpeg.body())).getWidth()==1280,"Photo uses still size");
            check(send(http,base,"POST","/api/command",token,Json.encode(cmd("cameraStop","test",5)),base).statusCode()==200,"Camera stop");
            check(send(http,base,"GET","/api/camera/frame?after=0",token,null,null).statusCode()==204,"Stopped camera invalidates latest frame");
            check(send(http,base,"GET","/%2e%2e/API.md",null,null,null).statusCode()==404,"Static traversal rejected");
        } finally { Files.deleteIfExists(root.resolve("index.html"));Files.deleteIfExists(root); }
    }
    private static HttpResponse<byte[]> send(HttpClient client,String base,String method,String path,String token,String body,String origin) throws Exception {
        var r=HttpRequest.newBuilder(URI.create(base+path)).timeout(Duration.ofSeconds(8));
        if(token!=null)r.header("Authorization","Bearer "+token);
        if(origin!=null)r.header("Origin",origin);
        if(body!=null)r.header("Content-Type","application/json");
        r.method(method,body==null?HttpRequest.BodyPublishers.noBody():HttpRequest.BodyPublishers.ofString(body));
        return client.send(r.build(),HttpResponse.BodyHandlers.ofByteArray());
    }
}
