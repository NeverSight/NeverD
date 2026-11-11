import fixture.*;
public final class ScalarFieldCopyCaller {
    public static Target make(Provider owner, int input, long bits, float floating, double fraction) {
        return Core.make(owner, input, bits, floating, fraction);
    }
    public static Target makeStatic(Provider owner, int input, long bits, float floating, double fraction) {
        return Core.makeStatic(owner, input, bits, floating, fraction);
    }
}
