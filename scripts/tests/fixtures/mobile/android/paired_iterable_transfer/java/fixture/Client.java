package fixture;
import java.util.Map;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Client {
    public static Object call() {
        Trace.hit(9);
        Map<?, ?> result = Provider.merge(Trace.input, Trace.rows);
        Trace.after = result; Trace.hit(8);
        return result;
    }
}
