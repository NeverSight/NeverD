import fixture.*;
import java.lang.reflect.*;

public final class PlainInterfaceAncestorHarness {
    private static int identities, failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static void same(Object a, Object b) { check(a == b); identities++; }
    private static final class Child extends Receiver<Tag> {
        public Object context() { calls += 2; Trace.mark(3); return contextValue; }
    }
    private static Object call(Receiver<Tag> receiver, Object value, int mode) throws Exception {
        if (mode == 0) return receiver.identity(value);
        if (mode == 1) return ((ScopeRoot)receiver).identity(value);
        if (mode == 2) return ((Scope)receiver).identity(value);
        if (mode == 3) return ((Root<Tag>)receiver).identity(value);
        if (mode == 4) return ((Leaf<Tag>)receiver).identity(value);
        return ScopeRoot.class.getDeclaredMethod("identity",Object.class).invoke(receiver,value);
    }
    public static void main(String[] args) throws Exception {
        Receiver<Tag> receiver = new Receiver<>();
        Object first = new Object(), second = new Object();
        Object[] values = {first,second,null};
        for (Object context : values) for (Object value : values) for (int mode = 0; mode < 6; mode++) {
            receiver.contextValue = context;
            Trace.events = 0;
            int before = receiver.calls;
            same(call(receiver,value,mode),value);
            check(receiver.calls == before + 1 && Trace.events == 12);
        }
        Tag a = new Tag(), b = new Tag();
        Root raw = receiver;
        for (Tag value : new Tag[] {a,b,null}) {
            same(receiver.echo(value),value);
            same(((Root<Tag>)receiver).echo(value),value);
            same(raw.echo(value),value);
            same(receiver.last,value);
            if (value != null) { value.value++; check(receiver.last.value == value.value); }
        }
        int before = receiver.calls;
        try { raw.echo(first); throw new AssertionError(); }
        catch (ClassCastException expected) { failures++; check(receiver.calls == before); }
        Child child = new Child(); child.contextValue = second;
        for (int mode = 0; mode < 6; mode++) {
            Trace.events = 0; before = child.calls;
            same(call(child,first,mode),first);
            check(child.calls == before + 2 && Trace.events == 132);
        }
        RuntimeException sentinel = new IllegalStateException("owned");
        receiver.failure = sentinel;
        for (int mode = 0; mode < 6; mode++) {
            Trace.events = 0; before = receiver.calls;
            try { call(receiver,first,mode); throw new AssertionError(); }
            catch (Throwable actual) {
                if (actual instanceof InvocationTargetException) actual = actual.getCause();
                same(actual,sentinel); failures++;
            }
            check(receiver.calls == before + 1 && Trace.events == 1);
        }
        check(Root.class.getGenericInterfaces()[0] == Scope.class);
        check(Leaf.class.getGenericInterfaces()[1] == Scope.class);
        check(Scope.class.getGenericInterfaces()[0] == ScopeRoot.class);
        check(Receiver.class.getDeclaredMethod("context").getGenericReturnType() == Object.class);
        check(Receiver.class.getMethod("identity",Object.class).getDeclaringClass() == ScopeRoot.class);
        check(ScopeRoot.class.getDeclaredMethod("identity",Object.class).isDefault());
        check(Receiver.class.getDeclaredMethod("echo",Tag.class).getGenericReturnType().getTypeName().equals("T"));
        System.out.println("identities=" + identities + ",failures=" + failures);
        System.out.println("plain-ancestor-slots=retained,full-generic-edges=retained,dispatch-and-effects=matched");
    }
}
