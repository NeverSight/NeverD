package fixture;
public class Outer<S> {
    public final S seed;
    public Outer<S> destination;
    public Outer(S seed) { this.seed = seed; }
    public class Member<T,V extends Vector> {
        private final T value;
        private final V vector;
        public Member(T value, V vector) {
            Trace.captured = this;
            Trace.hit(6);
            this.value = value;
            this.vector = vector;
            Trace.hit(7);
        }
        public T value() { return value; }
        public V vector() { return vector; }
    }
}
