package fixture;
import java.io.IOException;
public class Trace {
    public static int receivers,calls,overrides,preCalls,actions;
    public static long events;
    public static Provider source,actualReceiver,result,sentinelResult;
    public static Object captured,token,payload,prePayload;
    public static Runnable before,action;
    public static IOException failure;
    public static void hit(int event) { events=events*10+event; }
}
