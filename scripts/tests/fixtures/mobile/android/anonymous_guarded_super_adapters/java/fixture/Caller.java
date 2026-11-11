package fixture;
public final class Caller {
    public static boolean contains(Api<String> receiver, Object value) { return receiver.contains(value); }
    public static int index(Api<String> receiver, Object value) { return receiver.index(value); }
}
