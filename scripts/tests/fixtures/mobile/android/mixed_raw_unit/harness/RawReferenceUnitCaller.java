import fixture.*;
public final class RawReferenceUnitCaller {
    public static Object call(Core core, Object first, Object second, Object third) {
        return core.invoke(first, second, third);
    }
    public static void typed(Core core, Void first, Context second, int third) {
        core.invoke(first, second, third);
    }
}
