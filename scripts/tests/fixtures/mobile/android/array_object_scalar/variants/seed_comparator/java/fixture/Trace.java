package fixture;
public class Trace {
    public static int calls,failAt;
    public static long events;
    public static Object values,key,result,payload;
    public static java.util.Comparator<Object> expectedComparator;
    public static Object seenComparator;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event) {events=events*10+event;if(failAt==event)throw failure;}
}
