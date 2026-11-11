package fixture;
public final class Trace {
    public static long events, left, right;
    public static int divisor, failAt, steps, initializations;
    public static RuntimeException failure;
    public static ArithmeticException caught;
    public static Object partial;
    public static Runnable observer;
    public static void hit(int event) {
        events=events*10+event;
        steps++;
        observer.run();
        if(event==failAt)throw failure;
    }
}
