package fixture;
public final class Creator {
    public static Outer make(Input input, Object tag) {
        return new Outer(new Inner().append("owned ").append(input != null ? input.read() : "missing").text(), tag);
    }
    public static Outer withTag(Input input, Object tag) {
        return new Outer(new Inner(tag).append("owned ").append(input != null ? input.read() : "missing").text(), tag);
    }
    public static IllegalStateException failure(Object input) {
        return new IllegalStateException(new StringBuilder().append("owned ")
            .append(input != null ? input.getClass().getSimpleName() : "missing").toString());
    }
}
