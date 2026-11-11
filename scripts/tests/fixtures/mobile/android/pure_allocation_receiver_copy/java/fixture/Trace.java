package fixture;
public final class Trace {
    public static int events,count,failAt;
    public static Target created;
    public static RuntimeException failure;
    public static void hit(int stage) { events=events*10+stage;if(failAt==stage)throw failure; }
}
