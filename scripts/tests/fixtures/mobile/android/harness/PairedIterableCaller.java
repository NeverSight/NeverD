import fixture.Client;
import fixture.Provider;
import fixture.Trace;
import java.util.Map;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class PairedIterableCaller {
    public static Object call() { return Client.call(); }
    public static Object direct() {
        Trace.hit(9);
        Map<?, ?> result = Provider.merge(Trace.input, Trace.rows);
        Trace.after = result; Trace.hit(8);
        return result;
    }
}
