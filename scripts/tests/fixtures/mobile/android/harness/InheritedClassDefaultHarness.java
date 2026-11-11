import fixture.*;
import java.lang.reflect.*;

public class InheritedClassDefaultHarness {
    private static int identities;
    private static int failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    private static void fails(Runnable action, Throwable exact) {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (Throwable error) {
            if (exact == null) check(error instanceof ClassCastException);
            else check(error == exact);
            failures++;
        }
    }
    private static void reflection() throws Exception {
        for (Class<?> child : new Class<?>[]{Child.class, Bounded.class}) {
            TypeVariable<?> i = child.getTypeParameters()[0];
            TypeVariable<?> j = child.getTypeParameters()[1];
            ParameterizedType base = (ParameterizedType) child.getGenericSuperclass();
            ParameterizedType api = (ParameterizedType) child.getGenericInterfaces()[0];
            check(base.getRawType() == Middle.class && api.getRawType() == Contract.class);
            check(base.getActualTypeArguments()[0].equals(i) && base.getActualTypeArguments()[1].equals(j));
            check(api.getActualTypeArguments()[0].equals(i) && api.getActualTypeArguments()[1].equals(j));
            int publicMethods = 0;
            for (Method method : child.getDeclaredMethods())
                if (!Modifier.isPrivate(method.getModifiers())) publicMethods++;
            check(publicMethods == 0);
            check(child.getMethod("value", Object.class).getDeclaringClass() == Root.class);
            check(child.getMethod("lookup", Object.class).getDeclaringClass() == Root.class);
        }
        TypeVariable<?> r = Root.class.getTypeParameters()[0];
        TypeVariable<?> s = Root.class.getTypeParameters()[1];
        Method lookup = Root.class.getDeclaredMethod("lookup", Object.class);
        check(lookup.getGenericParameterTypes()[0].equals(r));
        ParameterizedType result = (ParameterizedType) lookup.getGenericReturnType();
        check(result.getRawType() == Cell.class && result.getActualTypeArguments()[0].equals(r)
              && result.getActualTypeArguments()[1].equals(s));
        check(Bounded.class.getTypeParameters()[0].getBounds()[0] == Tag.class);
        for (Class<?> owner : new Class<?>[]{Root.class, Middle.class, Child.class, Bounded.class})
            for (Method method : owner.getDeclaredMethods()) check(!method.isBridge());
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Exception {
        Child<Object, Object> child = new Child<>();
        Root<Object, Object> root = child;
        Middle<Object, Object> middle = child;
        Contract<Object, Object> api = child;
        Cell<Object, Object> pair = new Cell<>(new Object(), new Object());
        child.pair = pair;
        for (Object next : new Object[]{new Object(), null, new Object()}) {
            Object old = child.stored;
            same(api.value(next), old);
            same(child.stored, next);
            same(child.lastInput, next);
            same(root.lookup(next), pair);
            same(middle.lookup(next), pair);
            same(api.lookup(next), pair);
        }
        RuntimeException sentinel = new RuntimeException("owned failure");
        child.failure = sentinel;
        Object old = child.stored;
        int values = child.valueCalls;
        fails(() -> api.value(null), sentinel);
        check(child.valueCalls == values + 1 && child.stored == old && child.lastInput == null);
        fails(() -> root.value(null), sentinel);
        fails(() -> middle.value(null), sentinel);
        fails(() -> api.lookup(null), sentinel);
        fails(() -> root.lookup(null), sentinel);
        fails(() -> middle.lookup(null), sentinel);
        Bounded<Tag, Object> bounded = new Bounded<>();
        Tag first = new Tag();
        Tag second = new Tag();
        bounded.stored = first;
        bounded.pair = new Cell<>(first, new Object());
        Contract<Tag, Object> typed = bounded;
        same(typed.value(second), first);
        same(typed.lookup(first), bounded.pair);
        String wrong = new String("wrong raw argument");
        same(((Contract) bounded).value(wrong), second);
        check(((Root) bounded).stored == wrong && bounded.valueCalls == 2);
        fails(() -> { Tag cast = bounded.stored; check(cast == first); }, null);
        bounded.failure = sentinel;
        fails(() -> typed.lookup(first), sentinel);
        check(bounded.lookupCalls == 2 && bounded.lastInput == first);
        check(Trace.defaults == 0);
        reflection();
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=0");
        System.out.println("inherited-class-default=retained,class-provider-precedence=matched,default-effects=absent");
    }
}
