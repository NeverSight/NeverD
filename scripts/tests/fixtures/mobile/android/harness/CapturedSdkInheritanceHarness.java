import fixture.Owner;
import fixture.Cursor;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.AbstractCollection;
import java.util.Collection;
import java.util.Iterator;

public final class CapturedSdkInheritanceHarness {
    private static int values, identities, failures;
    private interface Operation { void run(); }
    private static void value(boolean condition) {
        if (!condition) throw new AssertionError("value " + values);
        ++values;
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity " + identities);
        ++identities;
    }
    private static void fail(Class<? extends Throwable> kind, Operation operation) {
        try { operation.run(); }
        catch (Throwable thrown) {
            if (thrown.getClass() != kind) throw new AssertionError("failure " + failures, thrown);
            ++failures;
            return;
        }
        throw new AssertionError("missing failure " + failures);
    }
    public static void main(String[] arguments) throws Exception {
        boolean recovered = arguments.length == 1 && arguments[0].equals("recovered");
        Class<?> viewClass = Class.forName("fixture.Owner$View");
        if (viewClass.getSuperclass() != AbstractCollection.class
                || viewClass.getDeclaredClasses().length != 0
                || viewClass.getGenericInterfaces().length != 0
                || viewClass.getTypeParameters().length != 0
                || Modifier.isStatic(viewClass.getModifiers()) != recovered
                || !viewClass.getGenericSuperclass().getTypeName().equals(
                    recovered ? "java.util.AbstractCollection<java.lang.Object>"
                              : "java.util.AbstractCollection<T>"))
            throw new AssertionError("reported captured class reflection");
        if (!Owner.class.getTypeParameters()[0].getBounds()[0].equals(Object.class)
                || !Owner.class.getDeclaredField("value").getGenericType().getTypeName().equals("T")
                || viewClass.getDeclaredMethod("size").getGenericReturnType() != int.class
                || !viewClass.getDeclaredMethod("iterator").getGenericReturnType().getTypeName().equals(
                    recovered ? "java.util.Iterator<java.lang.Object>" : "java.util.Iterator<T>"))
            throw new AssertionError("original and projected method reflection");
        if (viewClass.getMethod("add", Object.class).getDeclaringClass() != AbstractCollection.class
                || viewClass.getMethod("isEmpty").getDeclaringClass() != AbstractCollection.class
                || viewClass.getMethod("toArray", Object[].class).getTypeParameters().length != 1)
            throw new AssertionError("SDK bodies and method formal stay inherited");
        Owner<Object> owner = new Owner<Object>(new Object());
        Owner<Object>.View view = owner.create();
        Collection<Object> collection = view;
        Field capture = viewClass.getDeclaredField("this$0");
        capture.setAccessible(true);
        same(capture.get(view), owner);
        Object[] tokens = {new Object(), new String("first"), null, new String("last")};
        for (Object token : tokens) {
            owner.value = token;
            int before = owner.reads;
            value(view.size() == 1);
            value(collection.size() == 1);
            value(!collection.isEmpty());
            owner.current = new Cursor(token);
            value(collection.contains(token));
            value(owner.reads == before + 1);
            owner.current = new Cursor(token);
            Iterator<Object> iterator = view.iterator();
            same(iterator.next(), token);
            fail(java.util.NoSuchElementException.class, () -> iterator.next());
            value(owner.reads == before + 2);
            Object[] supplied = {new Object(), new Object()};
            owner.current = new Cursor(token);
            Object[] reused = collection.toArray(supplied);
            same(reused, supplied);
            same(reused[0], token);
            same(reused[1], null);
            value(owner.reads == before + 3);
            owner.current = new Cursor(token);
            Object[] allocated = collection.toArray();
            value(allocated.length == 1);
            same(allocated[0], token);
            value(owner.reads == before + 4);
            Object[] empty = new Object[0];
            owner.current = new Cursor(token);
            Object[] expanded = collection.toArray(empty);
            value(expanded != empty && expanded.length == 1);
            same(expanded[0], token);
            value(owner.reads == before + 5);
            owner.current = new Cursor(token);
            value(!collection.contains(new Object()));
            value(owner.reads == before + 6);
            owner.current = new Cursor(token);
            if (token instanceof String || token == null) {
                String[] strings = collection.toArray(new String[1]);
                same(strings[0], token);
                value(strings.getClass() == String[].class);
            } else {
                fail(ArrayStoreException.class, () -> collection.toArray(new String[1]));
            }
            value(owner.reads == before + 7);
            fail(UnsupportedOperationException.class, () -> collection.add(token));
            value(owner.reads == before + 7);
            fail(NullPointerException.class, () -> collection.toArray((Object[]) null));
            value(owner.reads == before + 7);
            owner.current = new Cursor(token);
            fail(UnsupportedOperationException.class, () -> collection.clear());
            value(owner.reads == before + 8);
            owner.current = new Cursor(token);
            Object[] functional = collection.toArray(length -> new Object[length]);
            value(functional.length == 1);
            same(functional[0], token);
            value(owner.reads == before + 9);
            owner.current = new Cursor(token);
            java.util.Spliterator<Object> split = collection.spliterator();
            Object[] seen = new Object[1];
            value(split.tryAdvance(entry -> seen[0] = entry));
            same(seen[0], token);
            value(!split.tryAdvance(entry -> { throw new AssertionError("extra entry"); }));
            value(owner.reads == before + 10);
            owner.current = new Cursor(token);
            value(!collection.removeIf(entry -> false));
            value(owner.reads == before + 11);
            owner.current = new Cursor(token);
            fail(UnsupportedOperationException.class, () -> collection.removeIf(entry -> true));
            value(owner.reads == before + 12);
            same(owner.value, token);
            same(capture.get(view), owner);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("sdk-bodies=inherited,captured-outer=mutable,method-formal=unchanged");
    }
}
