package fixture;

// The runner inserts the owned Kotlin compiler's frozen package-part metadata.
public final class ScopeFacade__ScopePartKt {
    public ScopeFacade__ScopePartKt() {}
    public static <E> void register(Target<E> target, E element) {
        target.watcher = new Watcher<E>() {
            public void accept(E value) {
                target.last = value;
                target.callback.accept(element);
            }
        };
    }
}
