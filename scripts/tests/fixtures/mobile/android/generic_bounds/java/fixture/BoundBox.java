package fixture;

public class BoundBox<V extends W, W extends CharSequence> {
    public <T extends V> T select(boolean first, T left, T right) {
        return first ? left : right;
    }
}
