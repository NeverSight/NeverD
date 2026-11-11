import fixture.*;
public final class PrivateInstanceFieldNamesCaller {
    public static Callback<Object,Object> call(Provider<Object> receiver, Callback<Object,Object> next, long bits) {
        return receiver.exchange(next, bits);
    }
}
