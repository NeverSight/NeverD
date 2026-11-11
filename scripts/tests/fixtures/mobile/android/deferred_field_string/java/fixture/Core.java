package fixture;
public final class Core {
    public static Target fromInstance(Owner owner) {
        Target value = new Target("owned allocation label", null, false, owner.value);
        Trace.lastOwner = owner;
        return value;
    }
    public static Target fromStatic() {
        return new Target("owned allocation label", null, false, Owner.shared);
    }
}
