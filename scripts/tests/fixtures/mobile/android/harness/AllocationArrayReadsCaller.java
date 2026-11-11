import fixture.*;
public class AllocationArrayReadsCaller {
    public static Target call(int entry, Provider p, int index, Object input) {
        if (entry == 0) return Core.create(p, index, input);
        if (entry == 1) return Core.guarded(p, index, input);
        return Core.locked(p, index, input);
    }
}
