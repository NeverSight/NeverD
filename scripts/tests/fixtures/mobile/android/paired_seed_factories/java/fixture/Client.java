package fixture;
import fixture.api.Facade;
import java.util.Map;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Client {
    public static Object call(Object first, Object second) {
        Map<?, ?> result = (Map) Facade.make(first, second);
        Trace.after = result; Trace.hit(5);
        return result;
    }
    public static String checkedKey(Object first, Object second) {
        Map<String, ?> result = (Map) Facade.make(first, second);
        Trace.after = result; Trace.hit(5);
        return result.keySet().iterator().next();
    }
    public static String checkedValue(Object first, Object second) {
        Map<?, String> result = (Map) Facade.make(first, second);
        Trace.after = result; Trace.hit(5);
        return result.get(Trace.key);
    }
}
