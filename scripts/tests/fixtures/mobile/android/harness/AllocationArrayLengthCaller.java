import fixture.*;
public final class AllocationArrayLengthCaller {
    public static Target call(int variant, Holder holder, int[] input, boolean flag) {
        if (variant == 0) return Core.direct(holder, input, flag);
        if (variant == 1) return Core.field(holder, input, flag);
        if (variant == 2) return Core.live(holder, input, flag);
        return Core.nested(holder, input, flag);
    }
}
