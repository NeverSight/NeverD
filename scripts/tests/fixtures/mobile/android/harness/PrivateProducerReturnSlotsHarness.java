import fixture.*;
import java.lang.reflect.*;

public class PrivateProducerReturnSlotsHarness {
    private static int values, identities, failures;
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    private static void count(int actual, int expected) { check(actual == expected); values++; }
    private static void effects(Parent<Object> value, int calls, int overrides) {
        count(value.calls, calls);
        if (value instanceof Child<?>) count(((Child<?>) value).overrides, overrides);
        if (value instanceof Grandchild<?>) count(((Grandchild<?>) value).levels, overrides);
    }
    private static void fails(Runnable action, RuntimeException sentinel) {
        try { action.run(); throw new AssertionError("missing original producer failure"); }
        catch (RuntimeException error) { same(error, sentinel); failures++; }
    }
    private static void exercise(Parent<Object> value) {
        for (int i = 0; i < 10; i++) {
            Leaf<Object> leaf = new Leaf<>(); leaf.payload = new Object(); value.value = leaf;
            Object wide = Caller.readWide(value);
            same(wide, leaf); same(((Leaf<?>) wide).payload, leaf.payload);
            effects(value, 2 * i + 1, i);
            Leaf<Object> narrow = Caller.readNarrow(value);
            same(narrow, leaf); same(narrow.payload, leaf.payload);
            effects(value, 2 * i + 2, i + 1);
        }
        value.value = null;
        same(Caller.readWide(value), null); effects(value, 21, 10);
        same(Caller.readNarrow(value), null); effects(value, 22, 11);
        RuntimeException sentinel = new IllegalStateException("owned producer failure");
        value.failure = sentinel;
        fails(() -> Caller.readWide(value), sentinel); effects(value, 23, 11);
        fails(() -> Caller.readNarrow(value), sentinel); effects(value, 24, 12);
    }
    public static void main(String[] args) throws Exception {
        exercise(new Parent<Object>());
        exercise(new Child<Object>());
        exercise(new Grandchild<Object>());
        String narrowName = args[0];
        for (Class<?> owner : new Class<?>[]{Api.class, Parent.class, Child.class, Grandchild.class}) {
            Method narrow = owner.getDeclaredMethod(narrowName);
            for (Method method : owner.getDeclaredMethods())
                if (method.getName().equals(narrowName) && method.getReturnType() == Leaf.class) narrow = method;
            check(narrow.getReturnType() == Leaf.class);
            check(!Modifier.isFinal(narrow.getModifiers()) && !narrow.isBridge() && !narrow.isSynthetic());
            String formal = owner == Api.class ? "A" : owner == Parent.class ? "T"
                : owner == Child.class ? "C" : "G";
            check(narrow.getGenericReturnType().getTypeName().equals("fixture.Leaf<" + formal + ">"));
            for (Method method : owner.getDeclaredMethods()) {
                if (method.getName().equals("__neverdThrow")) {
                    check(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
                    check(method.getReturnType() == RuntimeException.class && method.getParameterCount() == 1
                        && method.getParameterTypes()[0] == Throwable.class);
                    continue;
                }
                check(!method.isSynthetic() && !method.isBridge());
            }
        }
        Method wide = Parent.class.getDeclaredMethod("items");
        // The original broad final entry keeps its name and body. Reflection
        // lookup by parameters may select either original return overload.
        for (Method method : Parent.class.getDeclaredMethods())
            if (method.getName().equals("items") && method.getReturnType() == Base.class) wide = method;
        check(wide.getReturnType() == Base.class && Modifier.isFinal(wide.getModifiers()));
        check(wide.getGenericReturnType().getTypeName().equals("fixture.Base<T>"));
        Method producer = Parent.class.getDeclaredMethod("produce");
        check(Modifier.isPrivate(producer.getModifiers()) && Modifier.isFinal(producer.getModifiers()));
        check(producer.getGenericReturnType().getTypeName().equals("fixture.Leaf<T>"));
        check(Parent.class.getGenericSuperclass().getTypeName().equals("fixture.Ancestor<T>"));
        check(Parent.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Api<T>"));
        check(Child.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<C>"));
        check(Grandchild.class.getGenericSuperclass().getTypeName().equals("fixture.Child<G>"));
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("final-private-producer-and-narrow-virtual-dispatch=matched,source-names=reported");
    }
}
