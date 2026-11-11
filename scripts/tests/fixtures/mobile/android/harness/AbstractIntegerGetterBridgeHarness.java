import fixture.*;
import java.lang.reflect.*;

public final class AbstractIntegerGetterBridgeHarness {
    private static int values, identities, failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static final class Child extends Receiver<Object,String> {
        public int getCount() { Trace.events = Trace.events * 10 + 1; return super.getCount() ^ 0x1234; }
    }
    private static int call(Receiver<Object,String> receiver, int mode) throws Exception {
        if (mode == 0) return receiver.size();
        if (mode == 1) return ((Parent<Object>)receiver).size();
        if (mode == 2) return ((CountView<Object>)receiver).size();
        if (mode == 3) return ((Integer)Parent.class.getMethod("size").invoke(receiver)).intValue();
        return ((Integer)Parent.class.getMethod("getCount").invoke(receiver)).intValue();
    }
    public static void main(String[] args) throws Exception {
        Receiver<Object,String> receiver = new Receiver<>();
        Child child = new Child();
        for (int value : new int[] {-1,0,1,7,Integer.MIN_VALUE,Integer.MAX_VALUE,0x40000000,0x80000001})
            for (Receiver<Object,String> target : new Receiver[] {receiver,child}) for (int mode = 0; mode < 5; mode++) {
                target.value = value;
                Trace.events = 0;
                int before = target.calls;
                check(call(target,mode) == (target instanceof Child ? value ^ 0x1234 : value)); values++;
                check(target.calls == before + 1 && Trace.events == (target instanceof Child ? 12 : 2));
            }
        RuntimeException sentinel = new IllegalStateException("getter");
        Trace.failure = sentinel;
        for (Receiver<Object,String> target : new Receiver[] {receiver,child}) for (int mode = 0; mode < 5; mode++) {
            Trace.events = 0;
            int before = target.calls;
            try { call(target,mode); throw new AssertionError(); }
            catch (Throwable actual) {
                if (actual instanceof InvocationTargetException) actual = actual.getCause();
                check(actual == sentinel); identities++; failures++;
            }
            check(target.calls == before + 1 && Trace.events == (target instanceof Child ? 12 : 2));
        }
        Method size = Parent.class.getMethod("size");
        check(Modifier.isFinal(size.getModifiers()) && !Modifier.isAbstract(size.getModifiers()));
        check(size.isBridge() == !(args.length != 0 && args[0].equals("recovered")));
        check(!size.isSynthetic());
        check(Modifier.isAbstract(Parent.class.getMethod("getCount").getModifiers()));
        check(Receiver.class.getMethod("size").getDeclaringClass() == Parent.class);
        check(Parent.class.getGenericInterfaces()[0].getTypeName().equals("fixture.CountView<P>"));
        check(Receiver.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<S>"));
        check(Receiver.class.getGenericInterfaces()[0].getTypeName().equals("fixture.CountView<S>"));
        for (Method method : Receiver.class.getDeclaredMethods()) check(!method.getName().equals("size"));
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("abstract-target=retained,original-final-bridge-owner-and-virtual-dispatch=matched,bridge-flags=reported");
    }
}
