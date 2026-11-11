package fixture;
import fixture.api.Facade;
import java.util.List;
public final class Client {
    public static Object call() {
        Carrier<?> result = Facade.make(Trace.box);
        Trace.after = result; Trace.hit(4);
        return result;
    }
    @SuppressWarnings("unchecked")
    public static String checked() {
        Carrier<List<String>> result = Facade.make((Carrier)Trace.box);
        Trace.after = result; Trace.hit(4);
        return (String)((List<?>)result.touch()).get(0);
    }
}
