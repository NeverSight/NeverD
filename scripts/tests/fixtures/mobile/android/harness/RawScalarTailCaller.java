import fixture.Api;
import fixture.Sink;
public final class RawScalarTailCaller {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static Object call(Api api, Object value, Sink sink) { return api.emit(value, sink); }
}
