package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Caller {
    public static int typed(Api receiver, Api other) { return receiver.compareTo(other); }
    public static int erased(Comparable receiver, Object other) { return receiver.compareTo(other); }
}
