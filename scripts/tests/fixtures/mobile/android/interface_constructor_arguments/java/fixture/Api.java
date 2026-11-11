package fixture;

public interface Api {
    Api Z_FIRST = new Tile(true, -17, 0x0123456789ABCDEFL);
    Api A_SECOND = new Tile(false, Integer.MIN_VALUE, Long.MIN_VALUE);
    Api M_THIRD = new Tile(true, 0, 0L);
    Api B_FLOAT = new Tile(-0.0f, Double.NaN);
}
