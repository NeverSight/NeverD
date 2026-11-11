package fixture;
public final class DeferredFieldStringCaller {
    public static Target call(int shape, Owner owner) {
        return shape == 0 ? Core.fromInstance(owner) : Core.fromStatic();
    }
}
