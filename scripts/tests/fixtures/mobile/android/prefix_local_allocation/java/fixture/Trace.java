package fixture;
public final class Trace {
    public static long events, word;
    public static int failAt;
    public static Object input;
    public static Api local;
    public static Object partial;
    public static RuntimeException failure;
    public static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
}
