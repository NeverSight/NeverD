import fixture.*;
public final class SelfCarrierCaller {
    public static <T> Carrier<T, ? extends Api<T>> call(Api<T> value) { return value.result(); }
}
