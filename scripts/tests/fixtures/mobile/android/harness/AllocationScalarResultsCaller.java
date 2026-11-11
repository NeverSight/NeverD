import fixture.*;

public final class AllocationScalarResultsCaller {
    public static Target call(int entry, Provider provider, Object input) {
        if (entry == 0) return Core.create(provider, input);
        if (entry == 1) return Core.guarded(provider, input);
        return Core.locked(provider, input);
    }
}
