package fixture;
import java.lang.reflect.Constructor;
public final class Core {
    private Core() {}
    public static <Value> Constructor<Value> call(Class<Value> type, Class<?>... parts) {
        return Provider.select(type,parts);
    }
    public static Constructor checked(Object type, Object parts) {
        return call((Class)type,(Class<?>[])parts);
    }
    public static <Value> Constructor<Value> ordinary(Class<Value> type, Class<?>[] parts) {
        return call(type,parts);
    }
}
