package fixture;
public final class Maker {
    public Target make(Provider provider,Object seed) {
        Object current=seed;
        try {
            return new Target(current=provider.current() instanceof Checked
                    ? ((Checked)provider.current()).value() : null,provider.bits());
        } catch (RuntimeException failure) {
            Trace.observed=current;
            Trace.event(7);
            throw failure;
        } finally {
            Trace.event(8);
        }
    }
}
