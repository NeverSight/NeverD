package fixture;
public class Trace {
    public static int receivers,calls,overrides,preCalls,actions,observed;
    public static long events;
    public static boolean left,right;
    public static Provider source,actualReceiver;
    public static Context result;
    public static String nextContext,context;
    public static Object captured,payload,prePayload;
    public static Runnable before,action;
    public static void hit(int event){events=events*10+event;}
}
