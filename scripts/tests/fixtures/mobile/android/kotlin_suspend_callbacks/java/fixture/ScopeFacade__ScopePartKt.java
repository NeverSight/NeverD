package fixture;

// The runner inserts the owned Kotlin compiler's frozen package-part metadata.
public final class ScopeFacade__ScopePartKt {
    public ScopeFacade__ScopePartKt() {}
    public static <E> Target<E> repeat(Target<E> target, int count, kotlin.coroutines.CoroutineContext context, E element, kotlin.jvm.functions.Function2 action) {
        target.watcher = new Watcher<E>() {
            public void accept(E value) {
                E result = (E) action.invoke(value, null);
                target.context = context;
                target.last = result;
                for (int i = 0; i < count; ++i) target.callback.accept(element);
            }
        };
        return target;
    }
}
