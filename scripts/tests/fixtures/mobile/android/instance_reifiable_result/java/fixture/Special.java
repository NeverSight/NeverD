package fixture;

public final class Special extends Provider {
    public Special(Object marker) { super(marker); }
    @Override
    public <U> U read(String name) throws Failure {
        Trace.overrides++;
        Trace.specialReceiver = this;
        return super.<U>read(name);
    }
}
