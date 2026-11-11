import fixture.*;
import java.util.Map;
import java.lang.invoke.*;
public final class NominalGetterCaller {
    public static Object generic(Core receiver, Key key) { return ((Reader)receiver).get(key); }
    public static Object mapped(Core receiver, Object key) { return ((Map)receiver).get(key); }
    public static Object exact(Core receiver, Object key, int slot, String alias) throws Throwable {
        Class<?> parameter = slot < 2 ? Key.class : Object.class;
        Class<?> result = slot == 1 || slot == 2 ? Box.class : Object.class;
        return MethodHandles.publicLookup().findVirtual(Core.class, slot == 1 ? alias : "get",
            MethodType.methodType(result, parameter)).invoke(receiver, key);
    }
}
