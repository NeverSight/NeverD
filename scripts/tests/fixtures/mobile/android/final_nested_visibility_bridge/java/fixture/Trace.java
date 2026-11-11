package fixture;
public final class Trace {
    public static long events, number, numberArgument, result;
    public static Object receiver;
    public static Payload input, argument;
    public static Owner.Child current;
    public static Runnable receiverAction, argumentAction, action;
    private Trace() { }
    public static void hit(int value) { events = events * 10 + value; }
}
