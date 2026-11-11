import fixture.*;
public final class AllocationFieldChainCaller {
    public static Target create(int variant, Holder holder) {
        if (variant == 0) return Core.make(holder);
        if (variant == 1) return Core.live(holder);
        return Core.keepLink(holder);
    }
}
