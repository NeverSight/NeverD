import fixture.*;
import java.lang.reflect.*;

public class CovariantGetterChainHarness {
    private static int values, identities, failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static class Child extends Receiver<Object,String> {
        public Leaf<Object> getCreated() {
            Trace.events = Trace.events * 10 + 9;
            return super.getCreated();
        }
    }
    private static Object call(Receiver<Object,String> receiver, int mode) throws Exception {
        if (mode == 0) return receiver.create();
        if (mode == 1) return ((View<Object>)receiver).create();
        if (mode == 2) return ((View)receiver).create();
        for (Method method : Receiver.class.getDeclaredMethods())
            if (method.getName().equals("create") && method.getReturnType() == Base.class)
                return method.invoke(receiver);
        throw new AssertionError();
    }
    public static void main(String[] args) throws Exception {
        Object token = new Object();
        Leaf<Object> leaf = new Leaf<>();
        leaf.value = token;
        RuntimeException sentinel = new IllegalStateException("getter");
        for (Receiver<Object,String> receiver : new Receiver[] {new Receiver<>(),new Child()}) {
            for (int mode = 0; mode < 4; mode++) for (Leaf<Object> value : new Leaf[] {leaf,null}) {
                receiver.value = value;
                Trace.events = 0;
                int before = receiver.calls;
                check(call(receiver,mode) == value); identities++;
                if (value != null) { check(value.value == token); identities++; }
                check(receiver.calls == before + 1); values++;
                check(Trace.events == (receiver instanceof Child ? 91 : 1)); values++;
            }
            Trace.failure = sentinel;
            for (int mode = 0; mode < 4; mode++) {
                Trace.events = 0;
                int before = receiver.calls;
                try { call(receiver,mode); throw new AssertionError(); }
                catch (Throwable actual) {
                    if (actual instanceof InvocationTargetException) actual = actual.getCause();
                    check(actual == sentinel); identities++; failures++;
                }
                check(receiver.calls == before + 1); values++;
                check(Trace.events == (receiver instanceof Child ? 91 : 1)); values++;
            }
            Trace.failure = null;
        }
        Method entry = Receiver.class.getDeclaredMethod("create");
        check(entry.getReturnType() == Leaf.class && Modifier.isFinal(entry.getModifiers()));
        check(entry.getGenericReturnType().getTypeName().equals("fixture.Leaf<T>"));
        check(entry.isBridge() == (args.length != 0 && args[0].equals("original")));
        check(Receiver.class.getGenericInterfaces()[0].getTypeName().equals("fixture.View<T>"));
        check(Leaf.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Base<T>"));
        for (Method method : Receiver.class.getDeclaredMethods())
            if (method.getName().equals("create") && method.getReturnType() == Base.class)
                check(method.isBridge() && method.isSynthetic());
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("final-entry=retained,virtual-getter=matched,full-generic-reflection=retained,bridge-flags=reported");
    }
}
