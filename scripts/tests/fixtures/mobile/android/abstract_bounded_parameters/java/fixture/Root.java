package fixture;
public abstract class Root<A, B> {
    public int visits;
    protected abstract void read(String marker, int index, B box, long wide, boolean enabled);
    public void dispatch(String marker, int index, B box, long wide, boolean enabled) {
        visits++; read(marker, index, box, wide, enabled);
    }
}
