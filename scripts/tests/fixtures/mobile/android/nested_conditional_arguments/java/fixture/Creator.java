package fixture;
public final class Creator {
    public static Outer make(Input primary,Input fallback) {
        return new Outer(new Inner(primary.first()!=null?primary.first():fallback.first(),
                                   primary.second()!=null?primary.second():fallback.second()));
    }
}
