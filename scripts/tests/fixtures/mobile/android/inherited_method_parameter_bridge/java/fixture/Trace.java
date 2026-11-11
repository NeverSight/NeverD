package fixture;
public class Trace {
    public static int receivers,calls,leaves,futures,preCalls,actions,observed;
    public static long events;
    public static Leaf source;
    public static Parent actualReceiver;
    public static Object result;
    public static Class token;
    public static String key;
    public static Object payload,prePayload;
    public static Runnable before,action;
    public static void hit(int event){events=events*10+event;}
}
