package fixture;
import fixture.api.Facade;
import java.util.Map;

public final class Client {
    public static Object call() {
        Map<?, ?> result = Facade.make();
        Trace.after = result; Trace.hit(4);
        return result;
    }
    public static String checkedKey() {
        Map<String, ?> result = Facade.make();
        Trace.after = result; Trace.hit(4);
        return result.keySet().iterator().next();
    }
    public static String checkedValue() {
        Map<?, String> result = Facade.make();
        Trace.after = result; Trace.hit(4);
        return result.get(Trace.key);
    }
}
