import fixture.*;
public final class AllocationArrayArgumentCaller {
    public static Target create(int variant, Holder holder, int size, Object[] outside, int index, int tag) {
        if (variant == 0) return Core.literal(holder, size, outside, index, tag);
        if (variant == 1) return Core.sized(holder, size, outside, index, tag);
        return Core.mutate(holder, size, outside, index, tag);
    }
}
