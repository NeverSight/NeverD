package fixture;
import java.util.concurrent.atomic.AtomicReference;

public final class RawChild extends Parent<AtomicReference> {
    public RawChild() {}
    @Override
    protected AtomicReference<?> wrap(Object input) {
        calls++;
        events = events * 10 + 4;
        seenInput = input;
        if (failure != null) throw failure;
        return result;
    }
    public AtomicReference<?> produce(Object input) { return wrap(input); }
}
