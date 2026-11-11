package fixture;
public class Trace {
    public static int receivers,calls,overrides,preCalls,actions,observed,result,mask;
    public static long events;
    public static Provider source,actualReceiver;
    public static Context captured;
    public static Object payload,prePayload;
    public static Runnable before,action;
    public static void hit(int event){events=events*10+event;}
}
