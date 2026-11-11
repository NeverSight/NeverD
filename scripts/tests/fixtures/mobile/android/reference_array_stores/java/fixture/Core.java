package fixture;
public final class Core {
    public static Object[] direct(Object[] input, int index, Object value) {
        Object[] view = (String[]) input;
        Trace.hit(2); view[index] = Trace.read(view, value); Trace.hit(4);
        return view;
    }
    public static Object[] rows(Object[] input, int index, Object value) {
        Object[] view = (int[][]) input;
        Trace.hit(2); view[index] = Trace.read(view, value); Trace.hit(4);
        return view;
    }
    public static Object[] fresh(Object value, int length, int index) {
        Object[] view = new String[Trace.size(length)];
        view[index] = Trace.read(view, value); Trace.hit(4);
        return view;
    }
    public static Target nested(Object value, int length, int index, Object tag) {
        Object[] view;
        Target result = new Target(view = new String[Trace.size(length)],
            view[index] = Trace.read(view, value), tag);
        Trace.hit(4); return result;
    }
    public static Object[] loop(Object[] values) {
        Object[] view = new String[Trace.size(values.length)];
        for (int i = 0; i < values.length; ++i) {
            view[i] = Trace.read(view, values[i]); Trace.hit(4);
        }
        return view;
    }
}
