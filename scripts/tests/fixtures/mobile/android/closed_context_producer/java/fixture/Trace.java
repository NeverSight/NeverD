package fixture;
public class Trace {
    public static long events;
    public static int overrides;
    public static Closed<Context,Ticket> setup,inputSetup;
    public static Context context,inputContext;
    public static Producer<?> producer,inputProducer;
    public static Object value,produced,result;
    public static Ticket ticket;
    public static Runnable providerAfter,after;
    public static void hit(int value) { events=events*16+value; }
}
