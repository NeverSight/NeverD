import fixture.*;
public final class ClosedGetterCaller {
    public static Box<Throwable> read(Base value) { return value.items(); }
}
