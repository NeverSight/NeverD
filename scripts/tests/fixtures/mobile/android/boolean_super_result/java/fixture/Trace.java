package fixture;
public class Trace {
    public static long events;
    public static int normalizations,actions,failAt;
    public static Object raw,key,expected,update,payload,normalizationPayload;
    public static Base<?,?> receiver;
    public static String normalized;
    public static boolean result;
    public static Runnable normalizeAction,action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
