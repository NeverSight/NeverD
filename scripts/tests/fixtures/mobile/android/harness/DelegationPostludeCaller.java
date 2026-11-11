import fixture.Core;
public final class DelegationPostludeCaller {
    public static Core create(Object value) { return new Core(value); }
    public static Core create(Object value, int mode) { return new Core(value, mode); }
    public static Core create(Object value, int mode, long bits) { return new Core(value, mode, bits); }
}
