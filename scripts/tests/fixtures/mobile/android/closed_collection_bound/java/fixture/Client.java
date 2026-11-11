package fixture;
import java.util.ArrayList;
import java.util.Collection;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Client {
    public static Object call() {
        Trace.hit(9);
        Collection result = Provider.collect(Trace.text, Trace.destination);
        Trace.after = result; Trace.hit(8);
        return result;
    }
    public static ArrayList checked() {
        Trace.hit(9);
        Collection result = Provider.collect(Trace.text, Trace.destination);
        Trace.after = result; Trace.hit(8);
        return (ArrayList)result;
    }
}
