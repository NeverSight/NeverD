package fixture;
public abstract class Middle<E, F extends Container<E>> extends Parent<E, F> {
    protected abstract void read(String marker, int index, F box, long wide, boolean enabled);
}
