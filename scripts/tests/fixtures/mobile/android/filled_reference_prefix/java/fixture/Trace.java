package fixture;
public class Trace {
    public static long events;
    public static Object first,second;
    public static Object[] supplied,replacement;
    public static boolean substitute;
    public static Base baseSelf;
    public static Core coreSelf;
    public static Runnable initialize,prepare,baseAction,after;
    public static void hit(int stage) { events=events*16+stage; }
}
