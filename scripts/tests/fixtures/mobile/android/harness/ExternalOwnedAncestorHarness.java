import fixture.*;
import fixture.base.Base;
import fixture.base.Root;
import java.lang.reflect.*;
import java.util.function.IntSupplier;

public class ExternalOwnedAncestorHarness {
    private static int identities;
    private static int failures;

    private static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }

    private static void same(Object actual, Object expected) {
        check(actual == expected);
        identities++;
    }

    private static void fails(Runnable action, Class<? extends Throwable> kind,
                              Throwable exact, IntSupplier calls, int expected,
                              int defaults) {
        try {
            action.run();
            throw new AssertionError("missing failure");
        } catch (Throwable error) {
            check(kind.isInstance(error));
            if (exact != null) check(error == exact);
            check(calls.getAsInt() == expected && Trace.defaults == defaults);
            failures++;
        }
    }

    private static void arguments(Type type, Class<?> owner, Type... expected) {
        check(type instanceof ParameterizedType);
        ParameterizedType actual = (ParameterizedType) type;
        check(actual.getRawType() == owner);
        Type[] found = actual.getActualTypeArguments();
        check(found.length == expected.length);
        for (int i = 0; i < found.length; ++i) check(found[i].equals(expected[i]));
    }

    private static void reflection() throws Exception {
        TypeVariable<?> p = Base.class.getTypeParameters()[0];
        TypeVariable<?> q = Base.class.getTypeParameters()[1];
        TypeVariable<?> i = Child.class.getTypeParameters()[0];
        TypeVariable<?> j = Child.class.getTypeParameters()[1];
        TypeVariable<?> s = Bounded.class.getTypeParameters()[0];
        TypeVariable<?> u = Bounded.class.getTypeParameters()[1];
        check(p.getGenericDeclaration() == Base.class && q.getGenericDeclaration() == Base.class);
        check(i.getGenericDeclaration() == Child.class && j.getGenericDeclaration() == Child.class);
        check(s.getGenericDeclaration() == Bounded.class && u.getGenericDeclaration() == Bounded.class);
        check(s.getBounds()[0] == Tag.class);
        arguments(Base.class.getGenericInterfaces()[0], Contract.class, p);
        arguments(Child.class.getGenericSuperclass(), Base.class, i, j);
        arguments(Bounded.class.getGenericSuperclass(), Base.class, s, u);
        Method lookup = Child.class.getDeclaredMethod("lookup", Object.class);
        check(Modifier.isProtected(lookup.getModifiers()) && !lookup.isBridge());
        check(lookup.getGenericParameterTypes()[0].equals(i));
        arguments(lookup.getGenericReturnType(), Cell.class, i, j);
        check(Child.class.getDeclaredMethod("value", Object.class).getGenericReturnType().equals(i));
        TypeVariable<?> r = Root.class.getTypeParameters()[0];
        TypeVariable<?> t = Root.class.getTypeParameters()[1];
        check(r.getGenericDeclaration() == Root.class && t.getGenericDeclaration() == Root.class);
        arguments(Base.class.getGenericSuperclass(), Root.class, p, q);
        check(Root.class.getDeclaredMethod("lookup", Object.class).getGenericParameterTypes()[0].equals(r));
        arguments(Root.class.getDeclaredMethod("fetch", Object.class).getGenericReturnType(), Cell.class, r, t);
        Method bridge = Bounded.class.getDeclaredMethod("value", Object.class);
        check(bridge.isBridge() && bridge.isSynthetic() && Modifier.isPublic(bridge.getModifiers()));
        check(bridge.getReturnType() == Object.class);
        check(Contract.class.getDeclaredMethod("relay", Object.class).isDefault());
        for (Class<?> owner : new Class<?>[]{Base.class, Child.class, Bounded.class}) {
            int count = 0;
            for (Method method : owner.getDeclaredMethods()) {
                check(!method.getName().equals("relay"));
                if (method.isBridge()) count++;
            }
            check(count == (owner == Bounded.class ? 1 : 0));
        }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Child<Object, Object> child = new Child<>();
        Base<Object, Object> base = child;
        Contract<Object> contract = child;
        Tag tag = new Tag();
        for (Object value : new Object[]{new Object(), tag, null}) {
            int before = child.calls;
            int defaults = Trace.defaults;
            same(child.value(value), value);
            same(base.value(value), value);
            same(contract.value(value), value);
            same(contract.relay(value), value);
            same(child.last, value);
            check(child.calls == before + 8 && Trace.defaults == defaults + 1);
        }
        Cell<Object, Object> cell = new Cell<>(tag, new Object());
        child.stored = cell;
        for (Object key : new Object[]{tag, new Object(), null}) {
            int before = child.calls;
            same(base.fetch(key), cell);
            same(child.last, key);
            check(child.calls == before + 3);
        }
        Object changed = new Object();
        cell.left = changed;
        same(base.fetch(tag), cell);
        same(child.stored.left, changed);
        RuntimeException sentinel = new RuntimeException("owned failure");
        child.failure = sentinel;
        int before = child.calls;
        int defaults = Trace.defaults;
        fails(() -> contract.relay(tag), RuntimeException.class, sentinel,
              () -> child.calls, before + 2, defaults + 1);
        before = child.calls;
        fails(() -> base.fetch(tag), RuntimeException.class, sentinel,
              () -> child.calls, before + 3, Trace.defaults);

        Bounded<Tag, Object> bounded = new Bounded<>();
        Base<Tag, Object> boundedBase = bounded;
        Contract<Tag> boundedContract = bounded;
        Contract raw = bounded;
        for (Tag value : new Tag[]{tag, null, new Tag()}) {
            before = bounded.calls;
            defaults = Trace.defaults;
            same(bounded.value(value), value);
            same(boundedBase.value(value), value);
            same(boundedContract.value(value), value);
            same(boundedContract.relay(value), value);
            same(bounded.last, value);
            check(bounded.calls == before + 16 && Trace.defaults == defaults + 1);
        }
        same(raw.value(tag), tag);
        same(raw.relay(null), null);
        Object incompatible = new Object();
        before = bounded.calls;
        defaults = Trace.defaults;
        fails(() -> raw.value(incompatible), ClassCastException.class, null,
              () -> bounded.calls, before, defaults);
        fails(() -> raw.relay(incompatible), ClassCastException.class, null,
              () -> bounded.calls, before, defaults + 1);
        bounded.failure = sentinel;
        defaults = Trace.defaults;
        fails(() -> raw.relay(tag), RuntimeException.class, sentinel,
              () -> bounded.calls, before + 4, defaults + 1);
        before = bounded.calls;
        defaults = Trace.defaults;
        fails(() -> raw.relay(incompatible), ClassCastException.class, null,
              () -> bounded.calls, before, defaults + 1);
        fails(() -> boundedBase.fetch(tag), RuntimeException.class, sentinel,
              () -> bounded.calls, before + 1, Trace.defaults);
        bounded.failure = null;
        bounded.calls = Integer.MAX_VALUE - 3;
        same(boundedContract.relay(tag), tag);
        check(bounded.calls == Integer.MIN_VALUE);

        Child<Object, Object> virtual = new Child<Object, Object>() {
            @Override public Object value(Object value) {
                calls++;
                return super.value(value);
            }
            @Override protected Cell<Object, Object> lookup(Object key) {
                calls++;
                return super.lookup(key);
            }
        };
        virtual.stored = cell;
        Contract<Object> virtualContract = virtual;
        Base<Object, Object> virtualBase = virtual;
        same(virtualContract.relay(tag), tag);
        same(virtualBase.fetch(tag), cell);
        check(virtual.calls == 7);
        reflection();
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=1");
        System.out.println("external-ancestors=retained,higher-bridge-dispatch=retained,external-default-dispatch=matched");
    }
}
