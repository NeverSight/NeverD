package fixture;
public class Trace {
    public static long events;
    public static int overrides;
    public static String[] labels,inputLabels;
    public static Context context,inputContext;
    public static Producer<?> producer,inputProducer;
    public static Object value,produced,result;
    public static String beforeLabel,replacement;
    public static Runnable providerAfter,after;
    public static void hit(int value) { events=events*16+value; }
}
