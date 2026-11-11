import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public class DisjointExternalDefaultsHarness {
    private static int identities;
    private static int failures;

    private static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }

    private static void same(Object actual, Object expected) {
        check(actual == expected);
        identities++;
    }

    private static void fails(Runnable action, Throwable expected) {
        try {
            action.run();
            throw new AssertionError("missing failure");
        } catch (Throwable error) {
            if (expected == null) check(error instanceof NullPointerException);
            else check(error == expected);
            failures++;
        }
    }

    private static void reflection() throws Exception {
        TypeVariable<?> k = CursorEntry.class.getTypeParameters()[0];
        TypeVariable<?> v = CursorEntry.class.getTypeParameters()[1];
        check(k.getGenericDeclaration() == CursorEntry.class && v.getGenericDeclaration() == CursorEntry.class);
        ParameterizedType iterator = (ParameterizedType) CursorEntry.class.getGenericInterfaces()[0];
        ParameterizedType entry = (ParameterizedType) CursorEntry.class.getGenericInterfaces()[1];
        check(iterator.getRawType() == Iterator.class && entry.getRawType() == Map.Entry.class);
        check(entry.getActualTypeArguments()[0].equals(k) && entry.getActualTypeArguments()[1].equals(v));
        check(iterator.getActualTypeArguments()[0].equals(entry));
        int bridges = 0;
        int publicMethods = 0;
        for (Method method : CursorEntry.class.getDeclaredMethods()) {
            if (!Modifier.isPrivate(method.getModifiers())) publicMethods++;
            if (method.isBridge()) {
                bridges++;
                check(method.getName().equals("next") && method.getReturnType() == Object.class);
            } else if (method.getName().equals("next")) {
                check(method.getGenericReturnType().equals(entry));
            }
        }
        check(bridges == 1 && publicMethods == 9);
        check(CursorEntry.class.getDeclaredMethod("getKey").getGenericReturnType().equals(k));
        check(CursorEntry.class.getDeclaredMethod("setValue", Object.class).getGenericReturnType().equals(v));
        check(CursorEntry.class.getMethod("forEachRemaining", java.util.function.Consumer.class)
              .getDeclaringClass() == Iterator.class);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        String key = new String("owned key");
        CursorEntry<String, Object> cursor = new CursorEntry<>(key, new Object());
        Map.Entry<String, Object> entry = cursor;
        Iterator<Map.Entry<String, Object>> iterator = cursor;
        Iterator raw = cursor;
        for (Object next : new Object[]{new Object(), null, new Object()}) {
            Object old = cursor.value;
            same(entry.getKey(), key);
            same(entry.getValue(), old);
            same(entry.setValue(next), old);
            same(entry.getValue(), next);
            same(cursor.next(), cursor);
            same(iterator.next(), cursor);
            same(raw.next(), cursor);
            cursor.consumed = false;
            iterator.forEachRemaining(item -> same(item, cursor));
        }
        check(cursor.nextCalls == 12 && cursor.hasNextCalls == 6 && cursor.consumed);
        check(entry.equals(cursor) && !entry.equals(new Object()) && entry.hashCode() == 17);
        RuntimeException sentinel = new RuntimeException("owned failure");
        cursor.failure = sentinel;
        fails(() -> cursor.next(), sentinel);
        fails(() -> iterator.next(), sentinel);
        fails(() -> raw.next(), sentinel);
        check(cursor.nextCalls == 15 && cursor.consumed);
        fails(() -> entry.getKey(), sentinel);
        check(cursor.keyCalls == 4);
        Object old = cursor.value;
        fails(() -> entry.setValue(new Object()), sentinel);
        check(cursor.setCalls == 4 && cursor.value == old);
        fails(() -> cursor.remove(), sentinel);
        check(cursor.removeCalls == 1 && cursor.consumed);
        cursor.failure = null;
        cursor.consumed = false;
        iterator.forEachRemaining(item -> { check(item == cursor); });
        check(cursor.nextCalls == 16 && cursor.consumed);
        cursor.consumed = false;
        fails(() -> iterator.forEachRemaining(item -> { check(item == cursor); throw sentinel; }), sentinel);
        check(cursor.nextCalls == 17 && cursor.consumed);
        Iterator missing = null;
        fails(() -> missing.next(), null);
        check(cursor.nextCalls == 17);
        cursor.remove();
        check(cursor.removeCalls == 2 && !cursor.consumed);
        reflection();
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=1");
        System.out.println("disjoint-interface-roots=retained,inherited-default-dispatch=matched,entry-and-iterator-views=matched");
    }
}
