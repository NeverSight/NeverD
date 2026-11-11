package fixture;
public class Trace {
    public static long events;
    public static Context context,inputContext;
    public static Carrier<?> seed,result,inputSeed;
    public static Class<?> token,inputToken;
    public static boolean useAlternate;
    public static Runnable initialize,after;
    public static void hit(int stage) { events=events*16+stage; }
}
