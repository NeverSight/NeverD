import fixture.Core;

public final class ConstructorParameterReadCaller {
    public static Core make(boolean defaults, int value) {
        return defaults ? new Core() : new Core(value);
    }
}
