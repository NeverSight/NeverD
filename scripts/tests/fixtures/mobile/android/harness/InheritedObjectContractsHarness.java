import fixture.*;
import java.lang.reflect.*;

public class InheritedObjectContractsHarness {
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
        TypeVariable<?> owner = View.class.getTypeParameters()[0];
        check(owner.getName().equals("T") && owner.getGenericDeclaration() == View.class);
        Method copy = View.class.getDeclaredMethod("copy", Object[].class, Object.class);
        TypeVariable<?>[] formals = copy.getTypeParameters();
        check(formals.length == 2 && formals[0].getName().equals("T")
              && formals[1].getName().equals("U"));
        for (TypeVariable<?> formal : formals) {
            check(formal.getGenericDeclaration().equals(copy));
            check(formal.getBounds().length == 1 && formal.getBounds()[0] == Object.class);
        }
        check(!owner.equals(formals[0]));
        check(((GenericArrayType) copy.getGenericParameterTypes()[0])
              .getGenericComponentType().equals(formals[0]));
        check(copy.getGenericParameterTypes()[1].equals(formals[1]));
        check(((GenericArrayType) copy.getGenericReturnType())
              .getGenericComponentType().equals(formals[0]));
        Method arrays = View.class.getDeclaredMethod("toArray", Object[].class);
        TypeVariable<?> arrayFormal = arrays.getTypeParameters()[0];
        check(arrayFormal.getGenericDeclaration().equals(arrays));
        check(((GenericArrayType) arrays.getGenericReturnType())
              .getGenericComponentType().equals(arrayFormal));
        Method values = View.class.getDeclaredMethod("value", Object.class);
        check(values.getTypeParameters().length == 0);
        check(values.getGenericParameterTypes()[0].equals(owner));
        check(values.getGenericReturnType().equals(owner));
        ParameterizedType parent = (ParameterizedType) View.class.getGenericInterfaces()[0];
        check(parent.getRawType() == Contract.class && parent.getActualTypeArguments()[0].equals(owner));
        int publicMethods = 0;
        for (Method method : View.class.getDeclaredMethods()) {
            check(!method.isBridge());
            if (!Modifier.isPrivate(method.getModifiers())) publicMethods++;
        }
        check(publicMethods == 4);
        check(View.class.getMethod("equals", Object.class).getDeclaringClass() == Object.class);
        check(View.class.getMethod("hashCode").getDeclaringClass() == Object.class);
        check(View.class.getMethod("toString").getDeclaringClass() == Object.class);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        View<Object> view = new View<>();
        Contract<Object> contract = view;
        Object tag = new Object();
        Object other = new Object();
        for (Object[] array : new Object[][]{new String[]{"first"}, new Integer[]{1},
                                            new Object[]{new Object()}, null}) {
            same(view.copy(array, tag), array);
            same(view.lastTag, tag);
            same(contract.copy(array, other), array);
            same(view.lastTag, other);
            same(view.toArray(array), array);
            same(contract.toArray(array), array);
            same(view.value(tag), tag);
            same(contract.value(other), other);
        }
        String[] typed = {"alpha"};
        same(view.copy(typed, 7), typed);
        same(contract.toArray(typed), typed);
        Contract raw = view;
        same(raw.copy(typed, null), typed);
        check(view.copies == 10 && view.arrays == 9 && view.values == 8);
        RuntimeException sentinel = new RuntimeException("owned failure");
        check(!view.equals("label") && view.equalsOverloads == 1);
        check(contract.equals(view) && !contract.equals(new Object()) && !contract.equals(null));
        check(contract.hashCode() == System.identityHashCode(view));
        check(contract.toString().equals("fixture.View@" + Integer.toHexString(System.identityHashCode(view))));
        view.failure = sentinel;
        check(contract.equals(view) && contract.hashCode() == System.identityHashCode(view));
        fails(() -> view.equals("label"), sentinel);
        check(view.equalsOverloads == 2);
        fails(() -> view.copy(typed, tag), sentinel);
        check(view.copies == 11 && view.lastTag == tag && typed[0].equals("alpha"));
        fails(() -> contract.copy(null, other), sentinel);
        check(view.copies == 12 && view.lastTag == other);
        fails(() -> view.toArray(typed), sentinel);
        fails(() -> contract.toArray(null), sentinel);
        check(view.arrays == 11);
        fails(() -> view.value(tag), sentinel);
        fails(() -> contract.value(null), sentinel);
        check(view.values == 10);
        View<Object> missing = null;
        fails(() -> missing.copy(typed, tag), null);
        check(view.copies == 12);
        reflection();
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=0");
        System.out.println("inherited-object-contracts=retained,unrelated-overloads=retained,root-method-dispatch=matched");
    }
}
