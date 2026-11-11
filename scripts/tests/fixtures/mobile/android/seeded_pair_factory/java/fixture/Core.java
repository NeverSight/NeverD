package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static PairValue call(Object left, Object right) { return PairValue.make(left, right); }
    public static PairValue<Base,Base> typed(Object left, Object right) { return (PairValue)call(left, right); }
}
