package fixture;

public class Trace {
    public static Mapper mapper;
    public static Rule rule;
    public static Rule result;
    public static Runnable action;
    public static int events;

    public static void hit(int event) {
        events = events * 10 + event;
    }
}
