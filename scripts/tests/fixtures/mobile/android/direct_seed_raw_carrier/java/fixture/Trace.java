package fixture;
public final class Trace {
    public static long events;
    public static int constructors,actions,failAt;
    public static Node constructed,created,result;
    public static Object payload;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int stage){events=events*10+stage;if(stage==failAt)throw failure;}
}
