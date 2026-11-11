package fixture;
public final class Core {
    private Core() {}
    public static <Value> Object call(Object context, int selector, Value... values) {
        return Provider.forward(context, selector, values);
    }
    public static Object checked(Object context, int selector, Object values) {
        return call(context, selector, (Object[]) values);
    }
}
