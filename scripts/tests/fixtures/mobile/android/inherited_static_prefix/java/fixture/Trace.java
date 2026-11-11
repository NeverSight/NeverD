package fixture;
public final class Trace {
    public static long events, seedWide;
    public static int failAt, seedCode;
    public static Object seedToken, partial;
    public static RuntimeException failure;
    public static Error fatal;
    public static Runnable observer;
    public static int seedInt() { hit(1); return seedCode; }
    public static long seedLong() { hit(2); return seedWide; }
    public static Object seedObject() { hit(3); return seedToken; }
    public static void hit(int event) {
        events=events*10+event;
        observer.run();
        if (event==failAt) {
            if (fatal!=null) throw fatal;
            throw failure;
        }
    }
}
