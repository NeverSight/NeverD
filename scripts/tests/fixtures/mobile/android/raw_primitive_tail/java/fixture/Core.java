package fixture;
@SuppressWarnings("rawtypes")
public final class Core<T> implements Api {
    public Object emit(Object value, Sink tail) {
        return emit(((Boolean) value).booleanValue(), tail);
    }
    public final Object emit(boolean value, Sink<? super Token> tail) {
        Trace.targets++;
        Trace.value = value ? 1 : 0;
        Trace.tail = tail;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return tail.resume(Trace.token);
    }
}
