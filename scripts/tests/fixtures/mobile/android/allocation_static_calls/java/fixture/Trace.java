package fixture;
public final class Trace {
    public static int events, calls, constructors;
    public static Object seen, after;
    public static long stamp;
    public static double fraction;
    public static RuntimeException targetFailure, providerFailure, callFailure, constructorFailure;
}
