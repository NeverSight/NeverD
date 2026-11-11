package fixture;
public final class Core<T> implements Api<Payload> {
    public Object emit(Payload value, Sink<? super Token> sink) {
        Trace.step(1);
        Trace.seen = value;
        if (sink != null) sink.accept(Trace.token);
        Trace.step(3);
        return Trace.result;
    }
}
