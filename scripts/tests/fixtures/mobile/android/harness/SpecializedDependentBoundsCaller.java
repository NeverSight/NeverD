import fixture.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class SpecializedDependentBoundsCaller {
    public static Object[] erased(Api self, Object[] input, Object tag, Sink sink) {
        return self.copy(input, tag, sink);
    }
    public static Value[] typed(Object self, boolean nested, Value[] input, Value tag, Sink<Value> sink) {
        return nested ? ((Host.Worker)self).copy(input, tag, sink) : ((Core)self).copy(input, tag, sink);
    }
    public static void resume(Api self, Object tag, Sink sink) { self.resume(tag, sink); }
}
