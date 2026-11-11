import fixture.*;
public final class BidirectionalFactoryCaller {
    public static Carrier<Object,Bound> call(Callback<Object,Bound> first,Callback<Bound,Object> second) {
        return Provider.<Object,Bound>create(first,second);
    }
}
