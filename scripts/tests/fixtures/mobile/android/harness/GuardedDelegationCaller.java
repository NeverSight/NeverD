import fixture.Core;
public final class GuardedDelegationCaller {
    public static Core make(int level, Object token, int count, long wide, double number) {
        if (level == 0) return new Core(token, count, wide, number);
        if (level == 1) return new Core(token, count, wide);
        if (level == 2) return new Core(token, count);
        return new Core(token);
    }
}
