package fixture;

public enum Choice implements Flag {
    FIRST, SECOND;

    public String text(int value) {
        return value > 0 ? "positive" : "nonpositive";
    }
}
