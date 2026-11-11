package fixture;
public class Provider {
    public static <Entry> Entry[] pass(Entry[] values, int count, Class<?> token) {
        Trace.values = values; Trace.token = token; Trace.count = count;
        Trace.limit = 0; Trace.scale = 0.0; Trace.enabled = false;
        Trace.hit(1); Trace.action.run(); Trace.hit(2);
        if (Trace.inspect) Trace.kind = token.isArray();
        Trace.hit(3);
        return (Entry[]) Trace.result;
    }
    public static <Entry> Entry[] spread(Entry[] values, Class<?> token, long limit, double scale, boolean enabled) {
        Trace.values = values; Trace.token = token; Trace.count = 0;
        Trace.limit = limit; Trace.scale = scale; Trace.enabled = enabled;
        Trace.hit(4); Trace.action.run(); Trace.hit(5);
        if (Trace.inspect) Trace.kind = token.isArray();
        Trace.hit(6);
        return (Entry[]) Trace.result;
    }
}
