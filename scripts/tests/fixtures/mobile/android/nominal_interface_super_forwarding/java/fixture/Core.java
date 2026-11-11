package fixture;
public final class Core {
    public static Output call(Root owner, Input value) {
        Trace.hit(6);
        Output result = owner.convert(value);
        Trace.hit(0);
        return result;
    }
}
