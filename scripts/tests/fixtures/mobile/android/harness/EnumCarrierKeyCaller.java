package fixture;
public final class EnumCarrierKeyCaller {
    public static <Cell extends Enum<Cell>,Key> Cell call(Pair<Cell,Key> input, Key key) { return Core.call(input,key); }
    public static <Cell extends Enum<Cell>,Key> Cell fallback(Pair<Cell,Key> input, Key key, Cell fallback) { return Core.fallback(input,key,fallback); }
    public static Enum checked(Object input, Object key, Object fallback) { return Core.checked(input,key,fallback); }
}
