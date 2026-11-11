package fixture;
public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T extends Mark> void write(Context context, String key, java.util.List<? extends T> values) {
        Trace.hit(1, context, key, values, null, 0);
        context.before(key, values);
        int index = context.index;
        T item = (T)context.next;
        Trace.hit(2, context, key, values, item, index);
        ((java.util.List)values).set(index, item);
        Trace.hit(3, context, key, values, item, index);
    }
}
