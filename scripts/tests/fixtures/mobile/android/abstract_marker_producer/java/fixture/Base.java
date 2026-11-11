package fixture;
public abstract class Base {
    public Marker saved;
    public Base() {}
    @SuppressWarnings("unchecked")
    protected <T extends Marker> T relay(String key, Base context) {
        if (key == null || context != this) throw new IllegalStateException();
        return read();
    }
    protected abstract <T extends Marker> T read();
}
