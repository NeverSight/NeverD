import fixture.*;

public final class AllocationPrivateCallsCaller {
    public static Target call(int entry, Core receiver, Object input, long mask) {
        if (entry == 0) return Core.create(receiver, input, mask);
        if (entry == 1) return Core.guarded(receiver, input, mask);
        return Core.locked(receiver, input, mask);
    }
}
