import fixture.*;
public final class DependentMethodBoundsCaller {
    public static <T> T[] call(Api<T> self, T[] input, T tag, Sink<? super T> sink) {
        return self.copy(input, tag, sink);
    }
}
