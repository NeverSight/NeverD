package fixture;
public class Core extends Base {
    public final Object value;
    public final int number;
    public final long bits;
    public Object tail;
    static { Trace.step(1); Trace.holder.value = Trace.initial; Trace.step(2); }
    public Core(Object token, int count, long raw) {
        super();
        Trace.seen = this;
        Trace.observe(this, 4);
        value = token;
        Trace.observe(this, 5);
        number = count;
        bits = raw;
        Trace.observe(this, 6);
    }
    public Core(Object token, boolean flag) {
        this(token, 17, 0x8000000000000023L);
        Trace.observe(this, 7);
        setFlag(flag);
        Trace.observe(this, 10);
    }
    public Core(Object token, boolean flag, Holder holder) {
        this(Trace.read(holder, token), flag);
        Trace.observe(this, 11);
        tail = holder.value;
        Trace.observe(this, 12);
    }
    private static Object readPrivate(Holder holder, Object token) { return Trace.read(holder, token); }
    static Object readPackage(Holder holder, Object token) { return Trace.read(holder, token); }
    protected static Object readProtected(Holder holder, Object token) { return Trace.read(holder, token); }
    public Core(Object token, boolean flag, Holder holder, byte marker) {
        this(readPrivate(holder, token), flag);
        Trace.observe(this, 11);
        tail = holder.value;
        Trace.observe(this, 12);
    }
    public Core(Object token, boolean flag, Holder holder, char marker) {
        this(readPackage(holder, token), flag);
        Trace.observe(this, 11);
        tail = holder.value;
        Trace.observe(this, 12);
    }
    public Core(Object token, boolean flag, Holder holder, short marker) {
        this(readProtected(holder, token), flag);
        Trace.observe(this, 11);
        tail = holder.value;
        Trace.observe(this, 12);
    }
    public Core(Object token, boolean flag, Holder holder, int marker) {
        this(Prefix.readPackage(holder, token), flag);
        Trace.observe(this, 11);
        tail = holder.value;
        Trace.observe(this, 12);
    }
    public Core(Object token, boolean flag, Holder holder, long marker) {
        this(Prefix.readProtected(holder, token), flag);
        Trace.observe(this, 11);
        tail = holder.value;
        Trace.observe(this, 12);
    }
}
