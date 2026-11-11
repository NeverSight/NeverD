package fixture;
public final class Owner {
    public static <S, V> Object make(S result) { return new Worker<S, V>(result); }
    public static <S, V> Object another(S result) { return new Worker<S, V>(result); }
    @SuppressWarnings("rawtypes")
    public static final class Worker<S, V> implements Function {
        public S result;
        public View<Character> view;
        public int calls, reads;
        public char last;
        public RuntimeException failure;
        public Worker(S result) { this.result = result; }
        public S key(char input) {
            calls++;
            last = input;
            if (failure != null) throw failure;
            return result;
        }
        public Object key(Object input) { return key(((Character) input).charValue()); }
        public View<Character> source() { reads++; return view; }
    }
}
