import fixture.*;
public final class SameBoundKeyCaller {
    public static Element call(Provider provider,Key key) { return provider.get(key); }
}
