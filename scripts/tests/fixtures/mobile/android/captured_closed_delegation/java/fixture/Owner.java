package fixture;
public final class Owner {
    private final class Worker {
        static { Trace.hit(1); }
        public final int index;
        public final Callback<? super Value,? extends Value> callback;
        public final Value value;
        public long bits;
        private Worker(int index,Callback<? super Value,? extends Value> callback) {
            Trace.partial=this;Trace.hit(2);
            this.index=index;Trace.hit(3);
            this.callback=callback;Trace.hit(4);
            Value selected=callback.apply(Trace.value);
            Trace.hit(6);
            this.value=selected;Trace.hit(7);
        }
        public Worker(int index,long bits,Callback<? super Value,? extends Value> callback) {
            this(index,callback);
            Trace.hit(8);this.bits=bits;Trace.hit(9);
        }
        public Owner owner() { return Owner.this; }
    }
    public Object create(int index,long bits,Callback<? super Value,? extends Value> callback) {
        return new Worker(index,bits,callback);
    }
}
