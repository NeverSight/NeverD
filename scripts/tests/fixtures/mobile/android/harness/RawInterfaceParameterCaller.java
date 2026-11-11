import fixture.Api;
import fixture.Sink;
public final class RawInterfaceParameterCaller {
    public static Object call(Api api, Object value, Sink sink) { return api.emit(value, sink); }
}
