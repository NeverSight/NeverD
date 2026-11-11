package fixture;
public final class Owner<T extends UpperBound> {
    public Base make(T token,long wide,Sink<Object> completion) {
        return new Worker<T>(token,wide,completion);
    }
    @SuppressWarnings("unchecked")
    public T read(Base value) { return ((Worker<T>)value).read(); }
    public static final class Worker<T extends UpperBound> extends Base implements Reader<T> {
        public final T token;
        public final long wide;
        @SuppressWarnings({"unchecked","rawtypes"})
        public Worker(T token,long wide,Sink<? super Worker> completion) {
            this.token=token;this.wide=wide;
            super(1,(Sink)completion);Trace.hit(5);
        }
        public T read() { Trace.hit(4);return token; }
        public Base spawn(Sink<Object> completion) { return new Worker<T>(token,wide,completion); }
    }
}
