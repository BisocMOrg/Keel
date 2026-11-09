// SPDX-License-Identifier: MIT
package org.keel.demo;

/** Optional Linux-only integration smoke; the backend is explicitly SIMULATOR. */
public final class NativeSimulatorTests {
    public static void main(String[] args) throws Exception {
        try(RobotControl robot=new RobotControl("simulator",true);CameraWorker camera=new CameraWorker("mock")) {
            long seq=1;
            robot.command(Json.map("type","claim","clientId","native-test","seq",seq++),camera).join();
            robot.command(Json.map("type","arm","clientId","native-test","seq",seq++),camera).join();
            long deadline=System.nanoTime()+3_000_000_000L;
            while(!robot.robotState().get("state").equals("ARMED") && System.nanoTime()<deadline) {
                Thread.sleep(80);
                robot.command(Json.map("type","heartbeat","clientId","native-test","seq",seq++),camera).join();
            }
            if(!robot.robotState().get("state").equals("ARMED")) throw new AssertionError(robot.robotState());
            robot.command(Json.map("type","drive","clientId","native-test","seq",seq++,"left",150,"right",-150),camera).join();
            Thread.sleep(80);
            if(!robot.robotState().get("left").equals(150)) throw new AssertionError("Applied snapshot: "+robot.robotState());
            Thread.sleep(400);
            if(!robot.robotState().get("state").equals("DISARMED")) throw new AssertionError("Native deadline did not disarm: "+robot.robotState());
            // Even if the owner loop has not run the queued arm, a newer stop invalidates it.
            var pending=robot.command(Json.map("type","arm","clientId","native-test","seq",seq++),camera);
            robot.command(Json.map("type","stop","clientId","native-test","seq",seq++),camera).join();
            try { pending.join(); } catch(java.util.concurrent.CompletionException expected) { }
            Thread.sleep(200);
            if(!robot.robotState().get("state").equals("DISARMED")) throw new AssertionError("Queued arm revived stopped device");
            System.out.println("Native simulator: arming, applied drive snapshot, 350 ms deadman, queued-arm cancellation passed");
        }
    }
}
