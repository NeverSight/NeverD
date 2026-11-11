package fixture;
import fixture.api.Facade;
import java.util.List;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Client {
    public static Object call(Object[] values) {
        List<?> result = (List) Facade.make(values);
        Trace.after = result; Trace.hit(5);
        return result;
    }
    public static String checked(Object[] values) {
        List<String> result = (List) Facade.make(values);
        Trace.after = result; Trace.hit(5);
        return result.get(0);
    }
}
