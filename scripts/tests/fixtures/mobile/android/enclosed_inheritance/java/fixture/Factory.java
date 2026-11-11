package fixture;

public class Factory<T> {
    public static <T, U> Base<T> make(final Box<T, U> box) {
        return new Base<T>() {
            public T echo(T value, Option option) {
                ++box.calls;
                if (box.failure != null) throw box.failure;
                box.value = value;
                box.option = option;
                return box.value;
            }
            public Box<T, U> box() { return box; }
        };
    }
    public static <T extends Tag, U> Base<T> makeBounded(final Box<T, U> box) {
        return new Base<T>() {
            public T echo(T value, Option option) {
                ++box.calls;
                if (box.failure != null) throw box.failure;
                box.value = value;
                box.option = option;
                return box.value;
            }
            public Box<T, U> box() { return box; }
        };
    }
}
