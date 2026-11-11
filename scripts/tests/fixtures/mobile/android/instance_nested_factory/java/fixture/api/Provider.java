package fixture.api;
import fixture.*;
import java.util.List;
public class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public <T> Carrier<List<T>> make(Carrier<T> input) {
        Trace.hit(2); Trace.seen = input; Trace.seenReceiver = this;
        input.touch();
        ((int[])Trace.input)[0]++;
        Trace.hit(3);
        return (Carrier<List<T>>)Trace.result;
    }
}
