import fixture.*;
public final class RawPrimitiveTailCaller {
    public static Object call(Api target, Object value, Sink tail) {
        return target.emit(value, tail);
    }
    public static Object primitive(Core<?> target, boolean value, Sink<? super Token> tail) {
        return target.emit(value, tail);
    }
}
