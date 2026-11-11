import fixture.*;
public final class AllocationSuperCallCaller {
    public static Target create(Core owner, int variant) {
        return variant == 0 ? owner.make() : owner.live();
    }
}
