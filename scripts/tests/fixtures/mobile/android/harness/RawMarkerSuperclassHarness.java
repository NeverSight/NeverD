import fixture.*;
import java.lang.reflect.*;

public class RawMarkerSuperclassHarness {
    public static class Child extends Consumer {
        public int overrides;
        @Override public String get() {
            overrides++;
            return super.get();
        }
    }
    private static int values, identities, failures;
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    private static void count(int actual, int expected) { check(actual == expected); values++; }
    private static void fails(Runnable action, RuntimeException sentinel) {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (RuntimeException error) { same(error, sentinel); failures++; }
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Exception {
        Consumer consumer = new Consumer();
        Factory<Integer,Long,String> typed = consumer;
        Factory raw = consumer;
        Root<String> root = consumer;
        ArityView<String> arity = consumer;
        String hit = new String("owned identity");
        consumer.result = hit;
        same(consumer.get(), hit); same(typed.get(), hit); same(raw.get(), hit);
        count(consumer.calls, 3);
        consumer.result = null;
        same(consumer.get(), null); same(typed.get(), null); same(raw.get(), null);
        count(consumer.calls, 6);
        RuntimeException sentinel = new IllegalStateException("owned failure");
        consumer.failure = sentinel;
        fails(() -> consumer.get(), sentinel);
        fails(() -> typed.get(), sentinel);
        fails(() -> raw.get(), sentinel);
        count(consumer.calls, 9);
        count(root.arity(), 3); count(arity.arity(), 3); count(consumer.rootCalls, 2);
        consumer.rootFailure = sentinel;
        fails(() -> root.arity(), sentinel); count(consumer.rootCalls, 3);
        fails(() -> arity.arity(), sentinel); count(consumer.rootCalls, 4);
        Child child = new Child();
        Factory<Integer,Long,String> childApi = child;
        child.result = hit;
        same(childApi.get(), hit); same(child.get(), hit);
        count(child.calls, 2); count(child.overrides, 2);
        child.failure = sentinel;
        fails(() -> childApi.get(), sentinel); count(child.calls, 3); count(child.overrides, 3);
        Type superclass = Consumer.class.getGenericSuperclass();
        if (args[0].equals("original")) check(superclass == Root.class);
        else check(superclass.getTypeName().equals("fixture.Root<java.lang.String>"));
        check(Consumer.class.getGenericInterfaces()[0].getTypeName().equals(
            "fixture.Factory<java.lang.Integer, java.lang.Long, java.lang.String>"));
        check(Root.class.getGenericInterfaces()[0].getTypeName().equals("fixture.ArityView<P>"));
        check(ArityView.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Marker<A>"));
        check(Factory.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Marker<R>"));
        int bridges = 0;
        for (Method method : Consumer.class.getDeclaredMethods()) {
            if (method.getName().equals("__neverdThrow")) {
                check(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
                check(method.getReturnType() == RuntimeException.class
                    && method.getParameterCount() == 1 && method.getParameterTypes()[0] == Throwable.class);
                continue;
            }
            check(method.getName().equals("get") && method.getParameterCount() == 0);
            check(method.getGenericReturnType() == method.getReturnType());
            check(method.isBridge() == (method.getReturnType() == Object.class));
            check(method.isSynthetic() == method.isBridge());
            if (method.isBridge()) bridges++;
        }
        check(bridges == 1);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("marker-argument=reported,runtime-descriptors-and-dispatch=retained,ordered-effects=matched");
    }
}
