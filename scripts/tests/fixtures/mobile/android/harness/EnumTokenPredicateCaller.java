package fixture;
public final class EnumTokenPredicateCaller {
    public static <Cell extends Enum<Cell>> boolean call(Class<Cell> type, String key) { return Core.call(type,key); }
    public static boolean checked(Object type, Object key) { return Core.checked(type,key); }
    public static <Cell extends Enum<Cell>> boolean negate(Class<Cell> type, String key) { return Core.negate(type,key); }
}
