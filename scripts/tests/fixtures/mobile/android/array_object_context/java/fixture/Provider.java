package fixture;
public class Provider {
    public static <Entry> int measure(Entry[] values, Object context, int offset) {
        Trace.values = values; Trace.context = context; Trace.offset = offset;
        Trace.limit = 0; Trace.scale = 0.0; Trace.enabled = false;
        Trace.hit(1); Trace.action.run(); Trace.hit(2);
        return ((Integer) Trace.result).intValue();
    }
    public static <Entry> int measureWide(Entry[] values, Object context, long limit, double scale, boolean enabled) {
        Trace.values = values; Trace.context = context; Trace.offset = 0;
        Trace.limit = limit; Trace.scale = scale; Trace.enabled = enabled;
        Trace.hit(1); Trace.action.run(); Trace.hit(2);
        return ((Integer) Trace.result).intValue();
    }
}
