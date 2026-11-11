import fixture.Core;
public final class ScalarIntersectionArgumentCaller {
    public static void call(Core core, long number, int label, boolean enabled, double fraction) {
        core.run(number, label, enabled, fraction);
    }
}
