import fixture.*;
public final class FinalThrowCaller {
    public static Core create(Object value) { return new Core(value); }
    public static Object read() { return StaticCore.second; }
}
