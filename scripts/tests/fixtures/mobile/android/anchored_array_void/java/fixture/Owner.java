package fixture;
public final class Owner {
    static { Trace.hit(9); }
    private static <T> void append(Bucket<T> sink, T[] values) {
        Trace.hit(1);
        for (int index = 0; index < values.length; index++) {
            T value = values[index]; Trace.before = value; Trace.hit(2);
            sink.add(value);
            Trace.last = value; Trace.hit(3);
        }
        Trace.hit(4);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void call(Bucket sink, Object[] values) { append(sink, values); }
}
