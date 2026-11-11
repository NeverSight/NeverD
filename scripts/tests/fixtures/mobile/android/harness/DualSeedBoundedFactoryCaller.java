import fixture.*;
public final class DualSeedBoundedFactoryCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Carrier call(Object first, Converter converter, Carrier input,
            Object second, String label, Callback callback, int kind) {
        return Provider.create(first,converter,input,second,label,callback,kind);
    }
}
