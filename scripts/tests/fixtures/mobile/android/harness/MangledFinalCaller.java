import fixture.*;
public final class MangledFinalCaller {
    public static Object call(Terminal receiver, Object value) {
        return NamesKt.closed(receiver, value);
    }
}
