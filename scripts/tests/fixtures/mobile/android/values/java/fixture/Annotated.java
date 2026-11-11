package fixture;

@Values(number = 42, data = {"class", "tail"})
public final class Annotated {
    public static final int[] DEFAULTS = {2, 2, 0};
    @Values(number = 9)
    public final Object view;

    @Values(number = 11)
    public Annotated(Object value) {
        this.view = value;
    }

    @Values(number = 13, nested = @NestedValue(name = "method"))
    public int identity(@Values(number = 17) int value) {
        return value;
    }
}
