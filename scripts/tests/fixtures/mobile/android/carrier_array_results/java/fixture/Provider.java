package fixture;
public class Provider implements Facade {
    @SuppressWarnings("unchecked")
    public <T> T[] read(int begin, int end, Class<T> kind) {
        Trace.hit(1); Trace.self = this; Trace.begin = begin; Trace.end = end;
        Trace.token = kind; Trace.calls++; Trace.hit(2);
        Trace.callback.run(); Trace.hit(3); return (T[]) Trace.reply;
    }
}
