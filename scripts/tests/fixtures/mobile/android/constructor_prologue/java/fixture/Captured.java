package fixture;

public class Captured extends Base {
    public final int captured;
    public final Object token;
    public final int extra;

    public Captured(int value, Object input) {
        this.captured = value;
        this.token = input;
        super(3);
        extra = value + 3;
    }

    @Override
    public int value() { return captured; }
}
