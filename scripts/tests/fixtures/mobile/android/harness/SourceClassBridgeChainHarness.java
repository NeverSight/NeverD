import fixture.Child;
import fixture.Choice;
import fixture.Parent;
import fixture.Root;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class SourceClassBridgeChainHarness {
    private static int identities, failures, states;
    static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    static void state(boolean condition) { check(condition); states++; }
    public static void main(String[] args) throws Exception {
        String[] texts = {"owned first", "owned second", null, ""};
        Child<Choice,Choice> child = new Child<>();
        child.value = Choice.FIRST;
        Parent parentView = child;
        Root rootView = child;
        for (Choice chosen : new Choice[] {Choice.FIRST, Choice.SECOND, null}) {
            child.chosen = chosen;
            for (String text : texts) {
                int before = child.calls;
                same(child.parse(text), chosen);
                same(parentView.parse(text), chosen);
                same(rootView.parse(text), chosen);
                state(child.calls == before + 3 && child.parentCalls == 0 && child.value == Choice.FIRST);
            }
        }
        Parent<Choice> parent = new Parent<>();
        parent.value = Choice.SECOND;
        for (String text : texts) {
            int before = parent.parentCalls;
            same(parent.parse(text), Choice.SECOND);
            same(((Root) parent).parse(text), Choice.SECOND);
            state(parent.parentCalls == before + 2 && parent.value == Choice.SECOND);
        }
        RuntimeException sentinel = new IllegalStateException("owned failure");
        child.failure = sentinel;
        for (Choice chosen : new Choice[] {Choice.FIRST, Choice.SECOND, null}) {
            child.chosen = chosen;
            for (String text : texts) {
                int before = child.calls;
                try {
                    rootView.parse(text);
                    throw new AssertionError();
                } catch (RuntimeException actual) { same(actual, sentinel); failures++; }
                state(child.calls == before + 1 && child.parentCalls == 0 && child.chosen == chosen);
            }
        }
        try {
            ((Root) null).parse(null);
            throw new AssertionError();
        } catch (NullPointerException expected) { failures++; }
        check(((ParameterizedType) Parent.class.getGenericSuperclass()).getActualTypeArguments()[0]
                .equals(Parent.class.getTypeParameters()[0]));
        check(((ParameterizedType) Child.class.getGenericSuperclass()).getActualTypeArguments()[0]
                .equals(Child.class.getTypeParameters()[0]));
        check(Parent.class.getTypeParameters()[0].getBounds()[0] == java.io.Serializable.class);
        ParameterizedType bound = (ParameterizedType) Child.class.getTypeParameters()[0].getBounds()[0];
        check(bound.getRawType() == Enum.class && bound.getActualTypeArguments()[0].equals(Child.class.getTypeParameters()[0]));
        int parentBridges = 0, childBridges = 0, typed = 0;
        for (Method method : Parent.class.getDeclaredMethods()) {
            if (!method.getName().equals("parse")) continue;
            if (method.isBridge()) {
                check(method.isSynthetic() && method.getReturnType() == Object.class);
                parentBridges++;
            } else {
                check(method.getReturnType() == java.io.Serializable.class
                        && method.getGenericReturnType().equals(Parent.class.getTypeParameters()[0]));
            }
        }
        for (Method method : Child.class.getDeclaredMethods()) {
            if (!method.getName().equals("parse")) continue;
            if (method.isBridge()) {
                check(method.isSynthetic() && (method.getReturnType() == Object.class
                        || method.getReturnType() == java.io.Serializable.class));
                childBridges++;
            } else {
                check(method.getReturnType() == Enum.class
                        && method.getGenericReturnType().equals(Child.class.getTypeParameters()[0]));
                typed++;
            }
        }
        check(parentBridges == 1 && childBridges == 2 && typed == 1);
        System.out.println("identities=" + identities + ",states=" + states + ",failures=" + failures + ",bridges=3");
        System.out.println("parent-and-child-dispatch=matched,three-original-return-entries=retained,generic-scopes=retained");
    }
}
