package fixture;
public final class RawFactory {
    public static int calls;
    public static RawTag last;
    public static RuntimeException failure;
    public static RawBox<RawTag> make(RawTag input) {
        calls++;
        last = input;
        if (failure != null) throw failure;
        RawBox<RawTag> result = new RawBox<RawTag>();
        result.value = input;
        return result;
    }
}
