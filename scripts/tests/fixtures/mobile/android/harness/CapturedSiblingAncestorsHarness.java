import fixture.Cursor;
import fixture.Extended;
import fixture.Owner;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class CapturedSiblingAncestorsHarness {
    private static int values, identities, states, failures;
    private static void check(boolean condition) {
        ++values;
        if (!condition) throw new AssertionError("value case " + values);
    }
    private static void same(Object actual, Object expected) {
        ++identities;
        if (actual != expected) throw new AssertionError("identity case " + identities);
    }
    private static Object invoke(int route, Owner<Object>.Child child,
                                 String name, Object argument) throws Throwable {
        switch (route) {
        case 0:
            if (name.equals("hasNext")) return child.hasNext();
            if (name.equals("next")) return child.next();
            return child.echo((Object[]) argument);
        case 1: {
            Owner<Object>.Base base = child;
            if (name.equals("hasNext")) return base.hasNext();
            if (name.equals("next")) return base.next();
            return base.echo((Object[]) argument);
        }
        case 2: {
            Cursor<Object> cursor = child;
            if (name.equals("hasNext")) return cursor.hasNext();
            if (name.equals("next")) return cursor.next();
            return cursor.echo((Object[]) argument);
        }
        case 3: {
            Extended<Object> cursor = child;
            if (name.equals("hasNext")) return cursor.hasNext();
            if (name.equals("next")) return cursor.next();
            return cursor.echo((Object[]) argument);
        }
        case 4:
        case 5: {
            Class<?> owner = route == 4 ? Owner.Base.class : Owner.Child.class;
            Method method = name.equals("echo") ? owner.getMethod(name, Object[].class)
                                                   : owner.getMethod(name);
            try {
                return method.invoke(child, name.equals("echo")
                        ? new Object[]{argument} : new Object[0]);
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        default:
            if (name.equals("echo")) return MethodHandles.publicLookup().findVirtual(
                    Owner.Child.class, name, MethodType.methodType(Object[].class, Object[].class))
                    .invoke(child, argument);
            return MethodHandles.publicLookup().findVirtual(Owner.Child.class, name,
                    MethodType.methodType(name.equals("hasNext") ? boolean.class : Object.class))
                    .invoke(child);
        }
    }
    private static void state(Owner<?> owner, int calls, int marker) {
        ++states;
        if (owner.calls != calls || owner.marker != marker)
            throw new AssertionError("ordered state " + states);
    }
    public static void main(String[] arguments) throws Throwable {
        for (int route = 0; route < 7; ++route) {
            Object token = new Object(), replacement = new String("replacement");
            Owner<Object> owner = new Owner<>(token);
            Owner<Object>.Child child = owner.child(route + 1);
            Owner<Object>.Base alias = child;
            same(alias, child);
            check(child.tag == route + 1);
            int calls = 0, marker = 0;
            for (Object value : new Object[]{token, replacement, null, token}) {
                owner.value = value;
                check((Boolean) invoke(route, child, "hasNext", null) == (value != null));
                marker = marker * 3 + child.tag;
                state(owner, ++calls, marker);
                same(invoke(route, child, "next", null), value);
                marker = marker * 3 + child.tag;
                state(owner, ++calls, marker);
            }
            for (Object[] array : new Object[][]{new String[]{"first"}, new Object[]{token}, null}) {
                same(invoke(route, child, "echo", array), array);
                marker = marker * 3 + child.tag;
                state(owner, ++calls, marker);
                if (array != null) {
                    array[0] = replacement;
                    same(invoke(route, child, "echo", array), array);
                    marker = marker * 3 + child.tag;
                    state(owner, ++calls, marker);
                    same(array[0], replacement);
                }
            }
            Extended<Object> extended = child;
            extended.accept(replacement);
            marker = marker * 5 + child.tag;
            state(owner, ++calls, marker);
            same(alias.next(), replacement);
            marker = marker * 3 + child.tag;
            state(owner, ++calls, marker);
            RuntimeException expected = new IllegalStateException("original failure");
            owner.failure = expected;
            for (String name : new String[]{"hasNext", "next", "echo"}) {
                try { invoke(route, child, name, null); throw new AssertionError("missing failure"); }
                catch (IllegalStateException error) { same(error, expected); ++failures; }
                marker = marker * 3 + child.tag;
                state(owner, ++calls, marker);
            }
            try { extended.accept(token); throw new AssertionError("missing accept failure"); }
            catch (IllegalStateException error) { same(error, expected); ++failures; }
            marker = marker * 5 + child.tag;
            state(owner, ++calls, marker);
            same(owner.value, token);
            owner.failure = null;
            same(invoke(route, child, "next", null), token);
            marker = marker * 3 + child.tag;
            state(owner, ++calls, marker);
            try { invoke(route, null, "next", null); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expectedNull) { ++failures; }
            state(owner, calls, marker);
            check(Owner.Base.class.getMethod("echo", Object[].class).getTypeParameters().length == 1);
            check(Owner.Child.class.getMethod("echo", Object[].class).getDeclaringClass() == Owner.Base.class);
            check(Owner.Child.class.getMethod("next").getDeclaringClass() == Owner.Base.class);
            check(Cursor.class.isAssignableFrom(Owner.Child.class));
            check(Extended.class.isAssignableFrom(Owner.Child.class));
        }
        System.out.println("values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println("inherited-slots=original,outer=mutable,method-formal=independent");
    }
}
