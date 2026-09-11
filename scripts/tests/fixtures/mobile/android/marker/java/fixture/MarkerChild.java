package fixture;

public final class MarkerChild extends MarkerBase {
    public MarkerChild(int value) {
        super(value);
    }

    public int shifted(int value) {
        return adjust(value) + 5;
    }
}
