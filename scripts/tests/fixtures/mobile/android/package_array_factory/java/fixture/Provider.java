package fixture;
import java.util.List;
final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    static <T> List<T> make(T[] values) {
        Trace.hit(1);
        Trace.input = values;
        Trace.calls++;
        Trace.action.run();
        List<T> result = (List<T>) Trace.result;
        Trace.hit(2);
        return result;
    }
}
