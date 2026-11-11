package fixture;
public final class Provider {
    public static <T,V extends Bound> void consume(Source<T,V> source,State<T,V> state) {
        Trace.source=source;Trace.state=state;Trace.hit(1);
        state.value(source.value());
        state.vector(source.vector());
        state.tick(source.tick());
        state.running(source.running());
        Trace.hit(11);
    }
}
