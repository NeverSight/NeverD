package fixture;
import fixture.api.Facade;
import java.util.Map;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Client {
    public static Object call(Iterable<?> values) {
        Map<?, ?> result = (Map) Facade.make((Iterable) values);
        Trace.after = result; Trace.hit(6);
        return result;
    }
    public static String checkedKey(Iterable<?> values) {
        Map<String, ?> result = (Map) Facade.make((Iterable) values);
        Trace.after = result; Trace.hit(6);
        return result.keySet().iterator().next();
    }
    public static String checkedValue(Iterable<?> values) {
        Map<?, String> result = (Map) Facade.make((Iterable) values);
        Trace.after = result; Trace.hit(6);
        return result.get(Trace.key);
    }
}
