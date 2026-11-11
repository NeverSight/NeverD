import fixture.*;
public final class IntersectionCallbackWrongBoundHarness {
    public static void main(String[] args) {
        Object sentinel=new Object();int[] calls={0};
        Callback<Unit> callback=()->{calls[0]++;Trace.hit(6);return null;};
        Host host=new Host(callback);Trace.node=Trace.callback=sentinel;
        Throwable failure=null;
        try { host.run(); } catch(Throwable error) { failure=error; }
        if (!(failure instanceof ClassCastException) || Trace.events!=591 || Trace.getterCalls!=1
                || Trace.nodeCalls!=0 || calls[0]!=0 || Trace.node!=host || Trace.callback!=sentinel)
            throw new AssertionError("original late class/interface cast changed");
        System.out.println("original-late-cast=591");
    }
}
