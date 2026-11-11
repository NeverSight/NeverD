package fixture;
public class Trace {
    public static int tokenCalls,calls,actions,failAt;
    public static long events;
    public static Object token,type,value,fallback,payload,result;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int stage){events=events*10+stage;if(stage==failAt)throw failure;}
}
