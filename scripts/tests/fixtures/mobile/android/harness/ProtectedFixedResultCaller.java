import fixture.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedFixedResultCaller {
    public static Object call(Base self, Token token, int index, long wide, double fraction) { return self.dispatch(token, index, wide, fraction); }
}
