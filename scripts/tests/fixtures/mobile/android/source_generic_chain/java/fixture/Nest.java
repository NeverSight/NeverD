package fixture;

public class Nest {
    public Nest() {}

    public static class Cell<K, V> {
        public K left;
        public V right;

        public Cell(K left, V right) {
            this.left = left;
            this.right = right;
        }
    }

    public abstract static class Root<R, S> {
        public int calls;
        public R last;
        public RuntimeException failure;
        public Cell<R, S> stored;

        public Root() {}

        abstract Cell<R, S> lookup(R key);

        public Cell<R, S> fetch(R key) {
            return lookup(key);
        }
    }

    public static class Middle<P, Q> extends Root<P, Q> implements Contract<P> {
        public Middle() {}

        public P value(P value) {
            calls++;
            if (failure != null) throw failure;
            last = value;
            return value;
        }

        Cell<P, Q> lookup(P key) {
            calls++;
            if (failure != null) throw failure;
            last = key;
            return stored;
        }
    }

    public static class Child<I, J> extends Middle<I, J> {
        public Child() {}

        public I value(I value) {
            calls += 2;
            if (failure != null) throw failure;
            last = value;
            return value;
        }

        Cell<I, J> lookup(I key) {
            calls += 3;
            if (failure != null) throw failure;
            last = key;
            return stored;
        }
    }

    public static class Bounded<S extends Tag, U> extends Middle<S, U> {
        public Bounded() {}

        public S value(S value) {
            calls += 4;
            if (failure != null) throw failure;
            last = value;
            return value;
        }
    }
}
