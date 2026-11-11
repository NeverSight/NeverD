package fixture;

public enum Mode {
    BASE(3),
    CUSTOM(7) {
        @Override
        public int score(int value) { return value - bias(); }
    };

    private final int bias;
    private Mode(int bias) { this.bias = bias; }
    public int bias() { return bias; }
    public int score(int value) { return value + bias; }
}
