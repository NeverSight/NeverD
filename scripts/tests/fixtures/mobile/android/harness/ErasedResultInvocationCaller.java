import fixture.*;
public final class ErasedResultInvocationCaller {
    public static Value direct(Provider self, Object token, int index, long stamp, double fraction) {
        return self.read(token, index, stamp, fraction);
    }
    public static Item checked(Provider self, Object token, int index, long stamp, double fraction) {
        return self.read(token, index, stamp, fraction);
    }
    public static Box stat(Object token, int index, long stamp, double fraction) {
        return Provider.staticRead(token, index, stamp, fraction);
    }
}
