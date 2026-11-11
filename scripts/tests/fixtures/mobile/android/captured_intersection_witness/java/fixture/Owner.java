package fixture;
public class Owner extends Provider {
    public int index;
    public Object context() { Trace.hit(1); Trace.contextAction.run(); return Trace.context; }
    public void run(String[] values) {
        accept(new Member<Object>(context(), index, values));
        Trace.hit(8);
    }
    private class Member<T> extends Base<String> {
        Member(Object context, int index, String[] values) { super(context, index, values); Trace.hit(4); }
        public Object owner() { return Owner.this; }
    }
}
