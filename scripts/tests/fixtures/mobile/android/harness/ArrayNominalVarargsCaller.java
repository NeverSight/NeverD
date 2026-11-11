package fixture;
public final class ArrayNominalVarargsCaller {
    public static <Cell> Object call(Object context, int selector, Cell[] values) {
        return Core.call(context, selector, values);
    }
    public static Object checked(Object context, int selector, Object values) {
        return Core.checked(context, selector, values);
    }
}
