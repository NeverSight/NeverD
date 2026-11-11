import fixture.*;
public final class PrivateStaticFieldNamesCaller {
    public static Callback<Object,Object> call(Callback<Object,Object> next,long bits) { return Provider.exchange(next,bits); }
}
