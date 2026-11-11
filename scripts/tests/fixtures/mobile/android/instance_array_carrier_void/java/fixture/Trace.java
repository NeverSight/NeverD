package fixture;
public final class Trace {
    public static long events;
    public static int stores, overrides;
    public static Owner owner;
    public static Object receiver, usedCreator;
    public static Object[] array;
    public static Provider provider;
    public static Payload[] values;
    public static Contract.Creator<Payload> creator;
    public static Runnable receiverAction, arrayAction, creatorAction;
    private Trace() { }
    public static void hit(int value) { events = events * 10 + value; }
}
