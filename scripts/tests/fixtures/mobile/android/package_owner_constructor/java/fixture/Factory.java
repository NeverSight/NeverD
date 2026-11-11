package fixture;
public final class Factory {
    private Factory() {}
    public static Policy make(int hint) { Trace.hint=hint;Trace.step(9);return Trace.factoryPolicy; }
    public static void swap(Object value) { Pool.value=value; }
}
