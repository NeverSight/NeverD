package fixture;
import java.util.List;
public final class Client {
    public static Object call() {
        Carrier<?> result = Scope.make(Trace.receiver, Trace.box);
        Trace.after = result; Trace.hit(4);
        return result;
    }
    @SuppressWarnings("unchecked")
    public static String checked() {
        Carrier<List<String>> result = (Carrier)Scope.make(Trace.receiver, Trace.box);
        Trace.after = result; Trace.hit(4);
        return (String)Scope.read(result);
    }
}
