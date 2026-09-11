package fixture;

public final class MarkerImplementer implements MarkerInterface {
    public MarkerImplementer() {}

    public int value(int input) {
        return input + 11;
    }
}
