package fixture;
import java.util.Collection;

public final class EmptyCollectionCodec implements Api<Collection<?>> {
    public Collection<?> result, seenFallback;
    public Object seenInput;
    public int calls, events;
    public RuntimeException failure;
    public Error fatal;
    public EmptyCollectionCodec() {}
    @Override
    public Collection<?> convert(Object input, Collection<?> fallback) {
        calls++;
        events = events * 10 + 6;
        seenInput = input;
        seenFallback = fallback;
        if (failure != null) throw failure;
        if (fatal != null) throw fatal;
        return fallback == null ? result : fallback;
    }
}
