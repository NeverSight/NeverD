package fixture;
public class Trace {
    public static long events;
    public static Object first, second, result;
    public static Completion<?> completion;
    public static Runnable beforeAction, afterAction;
    public static void hit(int digit) { events = events * 10 + digit; }
    public static void enter(Object left, Object right, Completion<?> completion) {
        Trace.first = left;
        Trace.second = right;
        Trace.completion = completion;
        hit(1);
        if (beforeAction != null) beforeAction.run();
    }
    public static void tail(Object result) {
        Trace.result = result;
        hit(5);
        if (afterAction != null) afterAction.run();
        hit(7);
    }
}
