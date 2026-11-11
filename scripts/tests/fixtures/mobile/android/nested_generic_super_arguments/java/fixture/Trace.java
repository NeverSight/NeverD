package fixture;
public class Trace {
    public static long events, bits;
    public static int failAt;
    public static Object receiver, context, reply;
    public static Bound input;
    public static Result wrapper;
    public static Runnable action, failure;
    public static void hit(int event) {
        events=events*10+event;
        if(event==failAt) failure.run();
    }
}
