package fixture;

public final class Receiver implements Child {
    @Override public Object find(Object value) {
        return Trace.override(this, value);
    }
}
