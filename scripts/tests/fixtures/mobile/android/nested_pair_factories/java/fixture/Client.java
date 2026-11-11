package fixture;
import fixture.api.Facade;
import java.util.Map;
import java.util.List;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Client {
    public static Object call(Iterable<?> first, Iterable<?> second) {
        List<?> result = (List) Facade.make(first, second);
        Trace.after = result; Trace.hit(5);
        return result;
    }
    public static String checkedKey(Iterable<?> first, Iterable<?> second) {
        List<Map<String, ?>> result = (List) Facade.make(first, second);
        Trace.after = result; Trace.hit(5);
        return result.get(0).keySet().iterator().next();
    }
    public static String checkedValue(Iterable<?> first, Iterable<?> second) {
        List<Map<?, String>> result = (List) Facade.make(first, second);
        Trace.after = result; Trace.hit(5);
        return result.get(0).get(Trace.key);
    }
}
