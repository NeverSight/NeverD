import fixture.Base;
public final class ProtectedVoidCaller {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void call(Base receiver, Object value) { receiver.dispatch(value); }
}
