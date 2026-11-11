import fixture.*;
public final class UnseededNominalCarrierCaller {
    public static Result call(Pair<? extends Carrier<Object>,? extends Object> pair) { return Provider.<Object>make(pair); }
}
