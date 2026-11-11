import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public class ExternalGenericChainHarness {
    private static int identities;
    private static int failures;

    private static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }

    private static void same(Object actual, Object expected) {
        check(actual == expected);
        identities++;
    }

    private static void fails(Runnable action, Class<? extends Throwable> kind, Throwable exact) {
        try {
            action.run();
            throw new AssertionError("missing failure");
        } catch (Throwable error) {
            check(kind.isInstance(error));
            if (exact != null) check(error == exact);
            failures++;
        }
    }

    private static ParameterizedType arguments(Type type, Class<?> owner, Type... expected) {
        check(type instanceof ParameterizedType);
        ParameterizedType actual = (ParameterizedType) type;
        check(actual.getRawType() == owner);
        Type[] found = actual.getActualTypeArguments();
        check(found.length == expected.length);
        for (int i = 0; i < found.length; ++i) check(found[i].equals(expected[i]));
        return actual;
    }

    private static void reflection() throws Exception {
        TypeVariable<?> k = SetView.class.getTypeParameters()[0];
        TypeVariable<?> v = SetView.class.getTypeParameters()[1];
        check(k.getGenericDeclaration() == SetView.class && v.getGenericDeclaration() == SetView.class);
        ParameterizedType parent = (ParameterizedType) SetView.class.getGenericSuperclass();
        check(parent.getRawType() == AbstractSet.class);
        Type element = parent.getActualTypeArguments()[0];
        arguments(element, Map.Entry.class, k, v);
        arguments(SetView.class.getDeclaredMethod("iterator").getGenericReturnType(), Iterator.class, element);
        TypeVariable<?> e = Cursor.class.getTypeParameters()[0];
        arguments(Cursor.class.getGenericInterfaces()[0], Iterator.class, e);
        check(Cursor.class.getDeclaredMethod("next").getGenericReturnType().equals(e));
        TypeVariable<?> nk = Node.class.getTypeParameters()[0];
        TypeVariable<?> nv = Node.class.getTypeParameters()[1];
        arguments(Node.class.getGenericInterfaces()[0], Map.Entry.class, nk, nv);
        check(Node.class.getDeclaredMethod("getKey").getGenericReturnType().equals(nk));
        check(Node.class.getDeclaredMethod("setValue", Object.class).getGenericParameterTypes()[0].equals(nv));
        for (Class<?> owner : new Class<?>[]{SetView.class, Cursor.class, Node.class})
            for (Method method : owner.getDeclaredMethods()) check(!method.isBridge());
        check(Arrays.stream(SetView.class.getDeclaredMethods())
              .filter(method -> !Modifier.isPrivate(method.getModifiers())).count() == 2);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Exception {
        Object key = new Object();
        Object value = new Object();
        Node<Object, Object> node = new Node<>(key, value);
        SetView<Object, Object> view = new SetView<>(node, 1);
        AbstractCollection<Map.Entry<Object, Object>> collection = view;
        Set<Map.Entry<Object, Object>> set = view;
        same(node.getKey(), key);
        same(node.getValue(), value);
        same(view.iterator().next(), node);
        same(collection.iterator().next(), node);
        same(set.iterator().next(), node);
        check(collection.contains(node) && !collection.contains(new Object()));
        Object[] array = collection.toArray();
        check(array.length == 1);
        same(array[0], node);
        Map.Entry<Object, Object>[] typed = new Map.Entry[1];
        same(set.toArray(typed), typed);
        same(typed[0], node);
        check(set.hashCode() == 7 && set.equals(Collections.singleton(node)));
        set.forEach(entry -> same(entry, node));
        same(set.stream().findFirst().get(), node);
        set.spliterator().tryAdvance(entry -> same(entry, node));
        for (Object next : new Object[]{new Object(), null, value}) {
            Object old = node.value;
            same(node.setValue(next), old);
            same(node.getValue(), next);
            same(collection.toArray()[0], node);
        }
        view.count = 0;
        check(view.isEmpty());
        view.count = 1;
        check(!view.isEmpty());
        RuntimeException sentinel = new RuntimeException("owned failure");
        view.failure = sentinel;
        int allocations = Cursor.allocations;
        int iterators = view.iteratorCalls;
        fails(() -> set.iterator(), RuntimeException.class, sentinel);
        check(view.iteratorCalls == iterators + 1 && Cursor.allocations == allocations);
        int sizes = view.sizeCalls;
        fails(() -> set.size(), RuntimeException.class, sentinel);
        check(view.sizeCalls == sizes + 1 && Cursor.allocations == allocations);
        sizes = view.sizeCalls;
        fails(() -> collection.toArray(), RuntimeException.class, sentinel);
        check(view.sizeCalls == sizes + 1 && Cursor.allocations == allocations);
        view.failure = null;
        fails(() -> set.add(node), UnsupportedOperationException.class, null);
        Cursor<Map.Entry<Object, Object>> cursor = (Cursor<Map.Entry<Object, Object>>) view.iterator();
        cursor.failure = sentinel;
        fails(() -> cursor.next(), RuntimeException.class, sentinel);
        check(cursor.nextCalls == 1 && cursor.consumed && !cursor.hasNext());
        cursor.failure = null;
        cursor.consumed = false;
        fails(() -> cursor.forEachRemaining(entry -> { throw sentinel; }), RuntimeException.class, sentinel);
        check(cursor.nextCalls == 2 && cursor.consumed);
        reflection();
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=0");
        System.out.println("sdk-superclass=retained,ancestor-substitution=retained,inherited-collection-behavior=matched");
    }
}
