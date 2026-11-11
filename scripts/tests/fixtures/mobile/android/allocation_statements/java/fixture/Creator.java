package fixture;
public final class Creator {
    public static Target make(Object payload) {
        return new Target(switch (0) { default -> { Marker.before(Integer.MIN_VALUE, "owned-\u03bb"); yield Token.class; } }, payload);
    }
}
