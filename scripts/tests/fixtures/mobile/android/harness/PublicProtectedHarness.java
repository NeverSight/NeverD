import fixture.*;
import fixture.base.Base;
import java.lang.reflect.*;
import java.util.function.IntSupplier;

public class PublicProtectedHarness {
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
                              Throwable exact, IntSupplier calls, int expected) {
        try {
            action.run();
            throw new AssertionError("missing failure");
        } catch (Throwable error) {
            check(kind.isInstance(error));
            if (exact != null) check(error == exact);
            check(calls.getAsInt() == expected);
            failures++;
        }
    }

    private static void arguments(Type type, Type... expected) {
        check(type instanceof ParameterizedType);
        ParameterizedType actual = (ParameterizedType) type;
        check(actual.getRawType() == Base.class);
        Type[] found = actual.getActualTypeArguments();
        check(found.length == expected.length);
        for (int i = 0; i < found.length; ++i) check(found[i].equals(expected[i]));
    }

    private static void reflection() throws Exception {
        TypeVariable<?> p = Base.class.getTypeParameters()[0];
        TypeVariable<?> r = Base.class.getTypeParameters()[1];
        TypeVariable<?> s = Child.class.getTypeParameters()[0];
        TypeVariable<?> u = Child.class.getTypeParameters()[1];
        check(p.getGenericDeclaration() == Base.class && r.getGenericDeclaration() == Base.class);
        check(s.getGenericDeclaration() == Child.class && u.getGenericDeclaration() == Child.class);
        check(s.getBounds()[0] == Tag.class && u.getBounds()[0] == Tag.class);
        arguments(Child.class.getGenericSuperclass(), s, s);
        arguments(Closed.class.getGenericSuperclass(), Object.class, Tag.class);
        Method inherited = Base.class.getDeclaredMethod("value", Object[].class);
        check(Modifier.isProtected(inherited.getModifiers()) && Modifier.isAbstract(inherited.getModifiers()));
        check(inherited.getGenericReturnType().equals(r));
        check(inherited.getGenericParameterTypes()[0] instanceof GenericArrayType);
        check(((GenericArrayType) inherited.getGenericParameterTypes()[0]).getGenericComponentType().equals(p));
        Method typed = Child.class.getDeclaredMethod("value", Tag[].class);
        check(Modifier.isPublic(typed.getModifiers()) && typed.isVarArgs());
        check(typed.getGenericReturnType().equals(s));
        check(((GenericArrayType) typed.getGenericParameterTypes()[0]).getGenericComponentType().equals(s));
        Method closed = Closed.class.getDeclaredMethod("value", Object[].class);
        check(Modifier.isPublic(closed.getModifiers()) && closed.isVarArgs());
        check(closed.getReturnType() == Tag.class);
        check(closed.getGenericParameterTypes()[0] == Object[].class);
        for (Class<?> owner : new Class<?>[]{Child.class, Closed.class}) {
            int count = 0;
            for (Method method : owner.getDeclaredMethods()) {
                if (method.isBridge()) {
                    check(method.isSynthetic() && Modifier.isPublic(method.getModifiers()));
                    check(!method.isVarArgs() && method.getReturnType() == Object.class);
                    check(method.getParameterTypes()[0] == Object[].class);
                    count++;
                }
            }
            check(count == 1);
        }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Child<Specific, Tag> child = new Child<>();
        Specific a = new Specific();
        Specific b = new Specific();
        Base<Specific, Specific> typed = child;
        Base raw = child;
        for (Specific value : new Specific[]{a, b, null}) {
            Specific[] values = {value};
            same(child.value(values), value);
            same(typed.run(values), value);
            same(raw.run(values), value);
            same(child.last, value);
        }
        Tag tag = new Tag();
        same(raw.run(new Tag[]{tag}), tag);
        int before = child.calls;
        fails(() -> raw.run(new Object[]{a}), ClassCastException.class, null,
              () -> child.calls, before);
        fails(() -> raw.run(null), NullPointerException.class, null, () -> child.calls, before + 1);
        before = child.calls;
        fails(() -> raw.run(new Tag[0]), ArrayIndexOutOfBoundsException.class, null,
              () -> child.calls, before + 1);
        RuntimeException sentinel = new RuntimeException("owned failure");
        child.failure = sentinel;
        before = child.calls;
        fails(() -> raw.run(new Object[]{a}), ClassCastException.class, null,
              () -> child.calls, before);
        fails(() -> raw.run(null), RuntimeException.class, sentinel,
              () -> child.calls, before + 1);
        before = child.calls;
        fails(() -> typed.run(new Specific[]{b}), RuntimeException.class, sentinel,
              () -> child.calls, before + 1);
        child.failure = null;
        Specific[] mutable = {a};
        same(typed.run(mutable), a);
        mutable[0] = b;
        same(typed.run(mutable), b);

        Closed closed = new Closed();
        Base<Object, Tag> closedTyped = closed;
        Base closedRaw = closed;
        for (Tag result : new Tag[]{tag, a, null}) {
            closed.result = result;
            Object first = new Object();
            same(closed.value(first), result);
            same(closed.first, first);
            same(closedTyped.run(new Object[]{first}), result);
            same(closedRaw.run(new String[]{"owned value"}), result);
            same(closed.first, "owned value");
        }
        before = closed.calls;
        fails(() -> closedRaw.run(null), NullPointerException.class, null,
              () -> closed.calls, before + 1);
        before = closed.calls;
        fails(() -> closedRaw.run(new Object[0]), ArrayIndexOutOfBoundsException.class, null,
              () -> closed.calls, before + 1);
        closed.failure = sentinel;
        before = closed.calls;
        fails(() -> closedRaw.run(null), RuntimeException.class, sentinel,
              () -> closed.calls, before + 1);
        before = closed.calls;
        fails(() -> closedRaw.run(new Object[0]), RuntimeException.class, sentinel,
              () -> closed.calls, before + 1);

        Child<Specific, Tag> virtual = new Child<Specific, Tag>() {
            @Override public Specific value(Specific... values) {
                calls++;
                return super.value(values);
            }
        };
        Base virtualRaw = virtual;
        same(virtualRaw.run(new Specific[]{a}), a);
        check(virtual.calls == 2);
        fails(() -> virtualRaw.run(new Tag[]{tag}), ClassCastException.class, null,
              () -> virtual.calls, 2);
        reflection();
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=2");
        System.out.println("protected-parent=retained,public-targets-and-bridges=retained,generic-reflection=unchanged");
    }
}
