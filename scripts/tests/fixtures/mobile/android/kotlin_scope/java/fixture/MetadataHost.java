package fixture;

// The runner inserts the owned Kotlin compiler's frozen metadata annotation.
public final class MetadataHost {
    public MetadataHost() {}
    public static <X, Y> Box<Y> adapt(Box<X> input, Callback<X, Box<Y>> action) {
        Box<Y> result = new Box<Y>();
        input.watcher = new Watcher<X>() {
            public void accept(X value) { result.value = action.apply(value).value; }
        };
        return result;
    }
}
