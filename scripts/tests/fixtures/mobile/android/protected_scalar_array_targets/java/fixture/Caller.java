package fixture;
public final class Caller {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static int call(Base receiver, Object value) { return receiver.call(value); }
}
