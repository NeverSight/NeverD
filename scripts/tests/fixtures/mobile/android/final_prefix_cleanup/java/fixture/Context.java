package fixture;
public class Context {
    public Context() {}
    public Object first(Core owner, Object value) throws Throwable { Trace.hit(1, owner, value); return Trace.first; }
    public Object second(Core owner, Object value) throws Throwable { Trace.hit(2, owner, value); return Trace.second; }
    public void enter(Core owner) throws Throwable { Trace.hit(3, owner, null); }
    public boolean choose(Core owner, boolean choice) throws Throwable { Trace.hit(4, owner, null); return Trace.forceReject ? false : choice; }
    public void step(Core owner) throws Throwable { Trace.hit(5, owner, null); }
    public void cleanup(Core owner) throws Throwable { Trace.hit(6, owner, null); }
    public void after(Core owner) throws Throwable { Trace.hit(7, owner, null); }
}
