package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core<T> implements Api {
    public Object emit(Object value, Sink sink) {
        return emitTyped(((Scalar)value).decode(), sink);
    }
    public final Object emitTyped(long value, Sink<? super Token> sink) {
        Trace.step(2);
        Trace.accepted = value;
        if (sink != null) sink.accept(Trace.token);
        Trace.step(4);
        return Trace.result;
    }
}
