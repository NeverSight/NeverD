package fixture;

public final class Core {
    private Core() {}
    public static Holder call(Context context) {
        Trace.hit(1, context, null);
        Holder result = lookup(context);
        Trace.hit(4, context, result);
        return result;
    }
    @SuppressWarnings("unchecked")
    private static <T extends Base> Holder<T> lookup(Context context) {
        Trace.hit(2, context, null);
        Object candidate = Trace.fetch(context);
        if (candidate == null) throw Trace.missing(context);
        Holder<T> result = (Holder<T>) candidate;
        Trace.hit(3, context, result);
        return result;
    }
}
