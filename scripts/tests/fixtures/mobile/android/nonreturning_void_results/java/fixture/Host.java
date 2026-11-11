package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Host<T> {
    public T value;
    public Api<T> open() {
        return (Api) new Api<Object>() {
            public Void typedDrop(T input) {
                Effects.cause();
                throw new Failure();
            }
            public Void typedReplace(T input) {
                Effects.cause();
                throw new Failure();
            }
            public Void typedErase() {
                Effects.cause();
                throw new Failure();
            }
            public void add(Object input) { typedDrop((T) input); }
            public void set(Object input) { typedReplace((T) input); }
            public void remove() { typedErase(); }
            public Void narrowAdd(Object input) { return typedDrop((T) input); }
            public Void narrowSet(Object input) { return typedReplace((T) input); }
            public Void narrowRemove() { return typedErase(); }
            public Object read() { return Host.this.value; }
        };
    }
}
