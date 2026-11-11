import fixture.Core;
public final class PackageGenericDefaultsCaller {
    public static String run(int entry, Object input, String label) {
        if (entry == 0) return Core.create(input, label);
        if (entry == 1) return Core.guarded(input, label);
        return Core.locked(input, label);
    }
}
