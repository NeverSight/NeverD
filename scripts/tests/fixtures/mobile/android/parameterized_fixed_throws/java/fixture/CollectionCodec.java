package fixture;
import java.util.Collection;

public final class CollectionCodec implements Api<Collection<?>> {
    public Collection<?> result, seenFallback;
    public Object seenInput;
    public int calls, events;
    public java.io.IOException failure;
    public ReflectiveOperationException fatal;
    public CollectionCodec() {}
    @Override
    public Collection<?> convert(Object input, Collection<?> fallback)
            throws java.io.IOException, ReflectiveOperationException {
        calls++;
        events = events * 10 + 6;
        seenInput = input;
        seenFallback = fallback;
        if (failure != null) throw failure;
        if (fatal != null) throw fatal;
        return fallback == null ? result : fallback;
    }
}
