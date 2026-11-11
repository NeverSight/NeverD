package fixture;
public final class Creator {
    public static Target make(Object value, int selector) {
        Object captured = Trace.capture(value);
        int selected = Trace.select(selector);
        return new Target(captured, switch (selected) {
            case 0 -> Provider.choice(0);
            case 1 -> Provider.choice(1);
            case 2 -> Provider.choice(2);
            default -> throw new IllegalStateException(new StringBuilder().append("owned selector ").append(Provider.invalid(selected)).toString());
        });
    }
}
