import fixture.*;
public class AllocationArrayPayloadCaller {
    public static Target call(int entry, Object input) {
        if (entry == 0) return Core.create(input);
        if (entry == 1) return Core.guarded(input);
        return Core.locked(input);
    }
}
