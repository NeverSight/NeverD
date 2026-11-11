package fixture;
public final class ClosedVisibilityParameterBridgeCaller {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static <T extends First<Child>> void send(Child child, T value) {
        Trace.hit(1);
        ((Base) child).apply(value);
        Trace.hit(8);
    }
}
