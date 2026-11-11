package fixture;
public final class Core extends Base {
    public final Provider provider;
    public final int stamp;
    public Core(Provider provider) {
        super(provider.read() ? 0 : 1);
        Trace.hit(5); this.provider = provider; stamp = 17; Trace.hit(6);
    }
    protected int observe() { Trace.calls++; Trace.hit(4); return stamp; }
}
