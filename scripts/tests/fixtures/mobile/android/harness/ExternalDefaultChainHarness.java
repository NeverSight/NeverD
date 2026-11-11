import fixture.*;
import java.lang.reflect.*;
import java.util.function.IntUnaryOperator;

public class ExternalDefaultChainHarness {
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
        TypeVariable<?> formal = View.class.getTypeParameters()[0];
        check(formal.getName().equals("T") && formal.getGenericDeclaration() == View.class);
        ParameterizedType direct = (ParameterizedType) View.class.getGenericInterfaces()[0];
        check(direct.getRawType() == Leaf.class && direct.getActualTypeArguments()[0].equals(formal));
        check(View.class.getGenericInterfaces().length == 2);
        ParameterizedType redundant = (ParameterizedType) View.class.getGenericInterfaces()[1];
        check(redundant.getRawType() == Root.class && redundant.getActualTypeArguments()[0].equals(formal));
        ParameterizedType next = (ParameterizedType) Leaf.class.getGenericInterfaces()[0];
        check(next.getRawType() == Middle.class
              && next.getActualTypeArguments()[0].equals(Leaf.class.getTypeParameters()[0]));
        ParameterizedType root = (ParameterizedType) Middle.class.getGenericInterfaces()[0];
        check(root.getRawType() == Root.class
              && root.getActualTypeArguments()[0].equals(Middle.class.getTypeParameters()[0]));
        check(Leaf.class.getGenericInterfaces()[1] == Marker.class);
        check(Leaf.class.getGenericInterfaces().length == 3);
        ParameterizedType diamondRoot = (ParameterizedType) Leaf.class.getGenericInterfaces()[2];
        check(diamondRoot.getRawType() == Root.class
              && diamondRoot.getActualTypeArguments()[0].equals(Leaf.class.getTypeParameters()[0]));
        check(Marker.class.isAssignableFrom(View.class));
        Method value = View.class.getDeclaredMethod("value", Object.class);
        check(value.getTypeParameters().length == 0 && value.getGenericReturnType().equals(formal)
              && value.getGenericParameterTypes()[0].equals(formal));
        check(View.class.getMethod("fallback", int.class).getDeclaringClass() == Middle.class);
        check(View.class.getMethod("inheritedOnly", int.class).getDeclaringClass() == Root.class);
        check(Middle.class.getDeclaredMethod("fallback", int.class).isDefault());
        check(Root.class.getDeclaredMethod("fallback", int.class).isDefault());
        int publicMethods = 0;
        for (Method method : View.class.getDeclaredMethods()) {
            check(!method.isBridge());
            if (!Modifier.isPrivate(method.getModifiers())) publicMethods++;
        }
        check(publicMethods == 2);
        TypeVariable<?> childFormal = Child.class.getTypeParameters()[0];
        ParameterizedType parent = (ParameterizedType) Child.class.getGenericSuperclass();
        check(parent.getRawType() == View.class && parent.getActualTypeArguments()[0].equals(childFormal));
        for (Type edge : Child.class.getGenericInterfaces())
            check(((ParameterizedType) edge).getActualTypeArguments()[0].equals(childFormal));
        for (Method own : Child.class.getDeclaredMethods())
            check(Modifier.isPrivate(own.getModifiers()) && !own.isBridge());
        check(Child.class.getMethod("defaultValue", Object.class, Object.class).getDeclaringClass() == Root.class);
        check(Child.class.getMethod("reverse").getDeclaringClass() == Middle.class);
        int externalBridges = 0;
        for (Method own : Middle.class.getDeclaredMethods())
            if (own.getName().equals("reverse")) {
                if (own.isBridge()) {
                    check(own.isSynthetic() && own.getReturnType() == Root.class);
                    externalBridges++;
                } else {
                    check(own.getReturnType() == Middle.class);
                    ParameterizedType result = (ParameterizedType) own.getGenericReturnType();
                    check(result.getActualTypeArguments()[0].equals(Middle.class.getTypeParameters()[0]));
                }
            }
        check(externalBridges == 1);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Child<Object> child = new Child<>();
        View<Object> view = child;
        Leaf<Object> leaf = view;
        Middle<Object> middle = view;
        Root<Object> root = view;
        Root raw = view;
        for (Object value : new Object[]{new Object(), "label", 7, null}) {
            same(view.value(value), value);
            same(leaf.value(value), value);
            same(middle.value(value), value);
            same(root.value(value), value);
            same(raw.value(value), value);
        }
        check(view.calls == 20);
        for (Object value : new Object[]{new Object(), "fallback", 9, null}) {
            same(child.defaultValue(value, value), value);
            same(view.defaultValue(value, value), value);
            same(root.defaultValue(value, value), value);
            same(raw.defaultValue(value, value), value);
            check(Trace.lastArgument == value);
        }
        check(Trace.defaultCalls == 16);
        same(child.reverse(), child);
        same(view.reverse(), view);
        same(leaf.reverse(), view);
        same(middle.reverse(), view);
        same(root.reverse(), view);
        same(raw.reverse(), view);
        Root<Object> mirror = root.reverse();
        same(mirror, view);
        check(Trace.middleReverse == 7 && Trace.rootReverse == 0);
        check(view.fallback(7, 9) == 16 && view.overloads == 1);
        int i = 0;
        for (IntUnaryOperator action : new IntUnaryOperator[]{view::fallback, leaf::fallback,
                         middle::fallback, root::fallback, raw::fallback}) {
            check(action.applyAsInt(i) == i + 20);
            check(Trace.middle == ++i && Trace.lastOwner == 20 && Trace.rootFallback == 0);
        }
        i = 0;
        for (IntUnaryOperator action : new IntUnaryOperator[]{view::inheritedOnly, leaf::inheritedOnly,
                         middle::inheritedOnly, root::inheritedOnly, raw::inheritedOnly}) {
            check(action.applyAsInt(i) == i + 30);
            check(Trace.rootOnly == ++i && Trace.lastOwner == 30 && Trace.rootFallback == 0);
        }
        RuntimeException sentinel = new RuntimeException("owned failure");
        Trace.failure = sentinel;
        fails(() -> child.reverse(), sentinel);
        fails(() -> root.reverse(), sentinel);
        fails(() -> raw.reverse(), sentinel);
        Root<Object> absentReverse = null;
        fails(() -> absentReverse.reverse(), null);
        check(Trace.middleReverse == 10 && Trace.rootReverse == 0);
        fails(() -> child.defaultValue(child, null), sentinel);
        fails(() -> root.defaultValue(view, null), sentinel);
        fails(() -> raw.defaultValue(root, null), sentinel);
        check(Trace.defaultCalls == 19 && Trace.lastArgument == root);
        Child<Object> absent = null;
        fails(() -> absent.defaultValue(null, null), null);
        check(Trace.defaultCalls == 19);
        fails(() -> view.fallback(0), sentinel);
        fails(() -> root.fallback(1), sentinel);
        check(Trace.middle == 7 && Trace.lastOwner == 20 && Trace.rootFallback == 0);
        fails(() -> leaf.inheritedOnly(0), sentinel);
        fails(() -> root.inheritedOnly(1), sentinel);
        check(Trace.rootOnly == 7 && Trace.lastOwner == 30);
        view.failure = sentinel;
        fails(() -> view.value(null), sentinel);
        fails(() -> root.value(null), sentinel);
        check(view.calls == 22);
        fails(() -> view.fallback(7, 9), sentinel);
        check(view.overloads == 2 && Trace.middle == 7);
        View<Object> missing = null;
        fails(() -> missing.fallback(0), null);
        check(Trace.middle == 7);
        reflection();
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=0");
        System.out.println("external-default-chain=retained,nearest-default-owner=matched,dependency-effects=matched");
    }
}
