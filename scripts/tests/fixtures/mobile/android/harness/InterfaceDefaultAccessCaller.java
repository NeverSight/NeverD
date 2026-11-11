import fixture.*;
public final class InterfaceDefaultAccessCaller {
    public static Object call(DefaultApi receiver, Object value, String tag) {
        return DefaultApi.DefaultImpls.work(receiver, value, tag);
    }
}
