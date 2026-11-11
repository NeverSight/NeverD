package fixture;
import java.util.LinkedHashMap;
public final class EnumTokenMapCaller {
    public static <Cell extends Enum<Cell>> LinkedHashMap<String,Cell> call(Class<Cell> type) { return Core.call(type); }
    public static LinkedHashMap checked(Object type) { return Core.checked(type); }
    public static <Cell extends Enum<Cell>> boolean contains(Class<Cell> type, Object key) { return Core.contains(type,key); }
}
