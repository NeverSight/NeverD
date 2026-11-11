import fixture.*;
import java.lang.reflect.*;

public final class IndexedCovariantReturnHarness {
    private static int values, identities, failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static final class Child extends Receiver<Object,String> {
        public Leaf<Object> create(int lower, int upper) {
            Trace.events = Trace.events * 10 + 9;
            return super.create(lower, upper);
        }
    }
    private static Object call(Receiver<Object,String> receiver, int lower, int upper, int mode) throws Exception {
        if (mode == 0) return receiver.create(lower,upper);
        if (mode == 1) return ((Root<Object>)receiver).create(lower,upper);
        if (mode == 2) return ((RefinedIndex<Object>)receiver).create(lower,upper);
        if (mode == 3) return ((Indexed<Object>)receiver).create(lower,upper);
        if (mode == 4) return ((Root)receiver).create(lower,upper);
        if (mode == 5) {
            for (Method method : Receiver.class.getDeclaredMethods())
                if (method.getName().equals("create") && method.getReturnType() == Base.class)
                    return method.invoke(receiver,lower,upper);
        } else {
            for (Method method : RefinedIndex.class.getDeclaredMethods())
                if (method.getName().equals("create") && method.getReturnType() == Base.class)
                    return method.invoke(receiver,lower,upper);
            return Indexed.class.getMethod("create",int.class,int.class).invoke(receiver,lower,upper);
        }
        throw new AssertionError();
    }
    private static void exercise(Receiver<Object,String> receiver, Leaf<Object> leaf, Object token,
                                 int lower, int upper, int mode) throws Exception {
        receiver.value = leaf;
        Trace.events = 0;
        int before = receiver.calls;
        Object result = call(receiver,lower,upper,mode);
        check(result == leaf); identities++;
        if (leaf != null) { check(leaf.value == token); identities++; }
        check(receiver.lower == lower); values++;
        check(receiver.upper == upper); values++;
        check(receiver.calls == before + 1 && Trace.events == (receiver instanceof Child ? 91 : 1));
    }
    public static void main(String[] args) throws Exception {
        Receiver<Object,String> receiver = new Receiver<>();
        Child child = new Child();
        Leaf<Object> leaf = new Leaf<>();
        Object token = new Object();
        leaf.value = token;
        int[] indices = {-1,0,1,Integer.MIN_VALUE,Integer.MAX_VALUE};
        for (int lower : indices) for (int upper : indices) for (int mode = 0; mode < 7; mode++) {
            exercise(receiver,leaf,token,lower,upper,mode);
            exercise(child,leaf,token,lower,upper,mode);
        }
        for (int mode = 0; mode < 7; mode++) exercise(receiver,null,token,-3,5,mode);
        RuntimeException sentinel = new IllegalStateException("target");
        Trace.failure = sentinel;
        for (Receiver<Object,String> target : new Receiver[] {receiver,child}) for (int mode = 0; mode < 7; mode++) {
            Trace.events = 0;
            int before = target.calls;
            try { call(target,-3,5,mode); throw new AssertionError(); }
            catch (Throwable actual) {
                if (actual instanceof InvocationTargetException) actual = actual.getCause();
                check(actual == sentinel); identities++; failures++;
            }
            check(target.calls == before + 1 && Trace.events == (target instanceof Child ? 91 : 1));
            check(target.lower == -3); values++;
            check(target.upper == 5); values++;
        }
        check(Receiver.class.getGenericSuperclass().getTypeName().equals("fixture.Root<A>"));
        check(Receiver.class.getGenericInterfaces()[0].getTypeName().equals("fixture.RefinedIndex<A>"));
        check(RefinedIndex.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Indexed<P>"));
        check(Leaf.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Base<T>"));
        int bridges = 0;
        for (Method method : RefinedIndex.class.getDeclaredMethods())
            if (method.getReturnType() == Base.class) { check(method.isBridge() && method.isDefault()); bridges++; }
        check(bridges == (args.length != 0 && args[0].equals("omitted") ? 0 : 1));
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("full-indexed-result-substitution=retained,original-argument-order-and-dispatch=matched,interface-bridges=" + bridges);
    }
}
