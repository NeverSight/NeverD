package fixture;

public final class AdapterHost {
    private AdapterHost() {}
    public static final class Adapter implements Task {
        private final Callback<Input, Signal<? super Output>, Object> function;
        @SuppressWarnings("unchecked")
        public Adapter(Callback<? super Input, ? super Signal<? super Output>, ? extends Object> function) {
            this.function = (Callback) function;
        }
        public final Object run(Input input, Signal<? super Output> signal) {
            return function.call(input, signal);
        }
    }
}
