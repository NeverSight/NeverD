package fixture;
import fixture.api.Facade;
import java.util.List;
public final class Client {
    public static Object call() {
        List<?> result = Facade.make();
        Trace.after = result; Trace.hit(4);
        return result;
    }
    public static String checked() {
        List<String> result = Facade.make();
        Trace.after = result; Trace.hit(4);
        return result.get(0);
    }
}
