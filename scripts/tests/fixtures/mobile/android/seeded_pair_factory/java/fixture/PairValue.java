package fixture;
public class PairValue<F,S> {
    public F first;
    public S second;
    public PairValue() {}
    @SuppressWarnings("unchecked")
    public static <A,B> PairValue<A,B> make(A left, B right) {
        Trace.hit(1, left, right, null);
        PairValue<A,B> result = (PairValue<A,B>)Trace.next;
        Trace.hit(2, left, right, result);
        return result;
    }
}
