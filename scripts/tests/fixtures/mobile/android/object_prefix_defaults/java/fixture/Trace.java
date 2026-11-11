package fixture;
public class Trace {
    public static long events;
    public static Object argument;
    public static String label;
    public static View partial;
    public static Runnable guardAction, publishAction;
    public static void hit(int digit) { events = events * 10 + digit; }
    public static void guard(Object argument, String label) {
        Trace.argument = argument;
        Trace.label = label;
        hit(1);
        if (guardAction != null) guardAction.run();
        if (argument == null) throw new NullPointerException("owned argument");
    }
    public static void publish(View owner) {
        partial = owner;
        hit(2);
        if (publishAction != null) publishAction.run();
        hit(3);
    }
}
