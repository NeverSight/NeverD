import fixture.Child;
import fixture.Choice;
import fixture.Parent;
import fixture.Root;
import java.lang.reflect.Method;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class InheritedTypedProviderHarness {
    private static int identities, states, failures;
    static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    static void state(boolean condition) { check(condition); states++; }
    public static void main(String[] args) throws Exception {
        SourceClassBridgeChainHarness.main(args);
        Child<Choice,Choice> child = new Child<>();
        Parent parent = child;
        Root root = child;
        String[] texts = {"owned first", "owned second", null, ""};
        int[] indices = {0, 1, -1, Integer.MIN_VALUE, Integer.MAX_VALUE};
        for (Choice chosen : new Choice[] {Choice.FIRST, Choice.SECOND, null}) {
            for (String text : texts) for (int index : indices) {
                child.value = chosen;
                int reads = child.readCalls;
                same(child.read(text, index), chosen);
                same(parent.read(text, index), chosen);
                same(root.read(text, index), chosen);
                state(child.readCalls == reads + 3 && child.lastText == text && child.lastIndex == index);
                int writes = child.writeCalls;
                child.write(text, index, chosen);
                parent.write(text, index, chosen);
                root.write(text, index, chosen);
                state(child.writeCalls == writes + 3 && child.value == chosen
                        && child.lastText == text && child.lastIndex == index);
                try {
                    root.write(text, index, new Object());
                    throw new AssertionError();
                } catch (ClassCastException expected) { failures++; }
                state(child.writeCalls == writes + 3 && child.value == chosen);
            }
        }
        RuntimeException sentinel = new IllegalStateException("owned provider failure");
        child.value = Choice.FIRST;
        child.inheritedFailure = sentinel;
        for (String text : texts) for (int index : indices) {
            int reads = child.readCalls;
            try {
                root.read(text, index);
                throw new AssertionError();
            } catch (RuntimeException actual) { same(actual, sentinel); failures++; }
            state(child.readCalls == reads + 1 && child.value == Choice.FIRST
                    && child.lastText == text && child.lastIndex == index);
            int writes = child.writeCalls;
            try {
                root.write(text, index, Choice.SECOND);
                throw new AssertionError();
            } catch (RuntimeException actual) { same(actual, sentinel); failures++; }
            state(child.writeCalls == writes + 1 && child.value == Choice.FIRST
                    && child.lastText == text && child.lastIndex == index);
        }
        child.inheritedFailure = null;
        String pollution = "owned serializable value";
        int writes = child.writeCalls;
        root.write(null, Integer.MIN_VALUE, pollution);
        state(child.writeCalls == writes + 1 && parent.value == pollution);
        int reads = child.readCalls;
        same(root.read(null, Integer.MAX_VALUE), pollution);
        state(child.readCalls == reads + 1);
        try {
            Choice unused = child.read(null, -1);
            throw new AssertionError(unused);
        } catch (ClassCastException expected) { failures++; }
        state(child.readCalls == reads + 2 && parent.value == pollution);
        int finalReads = child.readCalls, finalWrites = child.writeCalls;
        try {
            ((Root) null).read(null, 0);
            throw new AssertionError();
        } catch (NullPointerException expected) { failures++; }
        try {
            ((Root) null).write(null, 0, null);
            throw new AssertionError();
        } catch (NullPointerException expected) { failures++; }
        state(child.readCalls == finalReads && child.writeCalls == finalWrites);
        int typedRead = 0, bridgedRead = 0, bridgedWrite = 0;
        for (Method method : Parent.class.getDeclaredMethods()) {
            if (method.getName().equals("read")) {
                if (method.isBridge()) {
                    check(method.isSynthetic() && method.getReturnType() == Object.class);
                    bridgedRead++;
                } else {
                    check(method.getReturnType() == java.io.Serializable.class
                            && method.getGenericReturnType().equals(Parent.class.getTypeParameters()[0]));
                    typedRead++;
                }
            }
            if (method.getName().equals("write") && method.isBridge()) {
                check(method.isSynthetic() && method.getParameterTypes()[2] == Object.class);
                bridgedWrite++;
            }
        }
        Method write = Parent.class.getDeclaredMethod("write", String.class, int.class, java.io.Serializable.class);
        check(write.getGenericParameterTypes()[2].equals(Parent.class.getTypeParameters()[0]));
        for (Method method : Child.class.getDeclaredMethods())
            check(!method.getName().equals("read") && !method.getName().equals("write"));
        check(typedRead == 1 && bridgedRead == 1 && bridgedWrite == 1);
        System.out.println("inherited-identities=" + identities + ",inherited-states=" + states
                + ",inherited-failures=" + failures + ",parent-bridges=2");
        System.out.println("parent-provider=retained,child-members=not-invented,bridge-casts-and-ordered-effects=matched");
    }
}
