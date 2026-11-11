package fixture;
public final class Factory<P, Q> {
    public final Object left, right, tag;
    public Factory(Object left, Object right, Object tag) {
        this.left = left; this.right = right; this.tag = tag;
    }
    @SuppressWarnings("unchecked")
    public static <X, Y> Factory<X, Y> make(Token<X> left, Token<Y> right, Context context) {
        Trace.hit(1, left, right, context);
        X first = left.read();
        Y second = right.read();
        Trace.hit(2, first, second, context);
        Factory<X, Y> result = (Factory<X, Y>)context.fetch();
        Trace.hit(3, result, first, second);
        return result;
    }
}
