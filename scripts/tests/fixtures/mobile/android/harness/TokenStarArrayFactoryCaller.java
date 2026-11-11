package fixture;
import java.lang.reflect.Constructor;
public final class TokenStarArrayFactoryCaller {
    public static <Cell> Constructor<Cell> call(Class<Cell> type, Class<?>... parts) { return Core.call(type,parts); }
    public static Constructor checked(Object type, Object parts) { return Core.checked(type,parts); }
    public static <Cell> Constructor<Cell> ordinary(Class<Cell> type, Class<?>[] parts) { return Core.ordinary(type,parts); }
}
