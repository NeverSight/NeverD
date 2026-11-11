package fixture;
public final class Trace {
    public static Object seenCell, cellValue;
    public static Carrier<?> cellToken, cellAlternative;
    public static int cellStamp;
    public static void beforeCell(Object value) {
        seenCell = value; cellValue = Scope.value(value); cellStamp = Scope.stamp(value); cellToken = Scope.token(value);
        if (cellAlternative != null) box = cellAlternative;
    }
    public static int events, failAt;
    public static RuntimeException failure;
    public static Object input, result, after, receiver, alternativeReceiver, seenReceiver;
    public static Carrier<?> box, seen, alternative;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == 2) { box = alternative; receiver = alternativeReceiver; }
        if (failAt == event) throw failure;
    }
}
