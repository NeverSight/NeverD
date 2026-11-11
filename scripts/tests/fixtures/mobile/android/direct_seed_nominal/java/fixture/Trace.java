package fixture;
public class Trace {
    public static int receivers,calls,overrides,preCalls,actions;
    public static long events;
    public static Provider source,actualReceiver,result;
    public static Object captured,payload,prePayload;
    public static Runnable before,action;
    public static void hit(int event){events=events*10+event;}
}
