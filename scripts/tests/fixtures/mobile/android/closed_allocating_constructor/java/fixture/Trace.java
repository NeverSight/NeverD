package fixture;
public final class Trace {
    public static Box<?> box;
    public static Core partial;
    public static Value value;
    public static Label label;
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
