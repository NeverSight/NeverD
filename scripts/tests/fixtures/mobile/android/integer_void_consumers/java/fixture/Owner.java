package fixture;

public final class Owner {
    public int calls;
    public int events;
    public int value;
    public Object lastReceiver;

    public Owner() {}

    @SuppressWarnings("rawtypes")
    public final class Receiver<S, V> implements Action {
        public int calls;
        public int value;
        public RuntimeException failure;

        public Receiver() {}

        public final void accept(int next) {
            calls++;
            value = next;
            Owner.this.calls++;
            Owner.this.value = next;
            Owner.this.events = Owner.this.events * 10 + 1;
            Owner.this.lastReceiver = this;
            if (failure != null) throw failure;
        }

        public void accept(Object next) {
            accept(((Number) next).intValue());
        }
    }
}
