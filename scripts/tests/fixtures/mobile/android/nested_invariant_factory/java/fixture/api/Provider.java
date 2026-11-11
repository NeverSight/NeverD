package fixture.api;
import fixture.*;
import java.util.List;
class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <T> Carrier<List<T>> make(Carrier<T> input) {
        Trace.hit(2); Trace.seen = input;
        input.touch();
        ((int[])Trace.input)[0]++;
        Trace.hit(3);
        return (Carrier<List<T>>)Trace.result;
    }
}
