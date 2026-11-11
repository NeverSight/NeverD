import fixture.*;
public final class CapturedGenericConstructorCaller {
    public static Child make(Object capture, Sink<? super Child> sink, long bits) {
        return new Child(capture, sink, bits);
    }
}
