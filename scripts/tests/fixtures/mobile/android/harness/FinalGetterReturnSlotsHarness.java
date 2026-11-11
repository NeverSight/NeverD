import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class FinalGetterReturnSlotsHarness {
    private static int identities, failures;
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static int methods(Class<?> owner, Set<String> helpers) {
        int count = 0;
        for (Method method : owner.getDeclaredMethods())
            if (!helpers.contains(owner.getName() + "->" + method.getName())) count++;
        return count;
    }
    private static Object invoke(Method method, Object receiver) throws Throwable {
        try { return method.invoke(receiver); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean original = args[0].equals("items");
        Set<String> helpers = new HashSet<>(Arrays.asList(args[1].split(",")));
        Method narrow = Receiver.class.getDeclaredMethod(args[0]);
        Method broad = Parent.class.getDeclaredMethod("items");
        check(narrow.getReturnType() == Leaf.class && broad.getReturnType() == Base.class);
        check(Modifier.isFinal(narrow.getModifiers()) && Modifier.isFinal(broad.getModifiers()));
        check(narrow.isBridge() == original && broad.isBridge() == original);
        check(!narrow.isSynthetic() && !broad.isSynthetic());
        check(narrow.getGenericReturnType().getTypeName().equals("fixture.Leaf<A>"));
        check(broad.getGenericReturnType().getTypeName().equals("fixture.Base<P>"));
        check(Receiver.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<A>"));
        check(Child.class.getGenericSuperclass().getTypeName().equals("fixture.Receiver<java.lang.Object, java.lang.String>"));
        int receiverGetters = 0;
        for (Method method : Receiver.class.getDeclaredMethods()) {
            if (method.getName().equals("getItems")) receiverGetters++;
            check(!method.getName().equals("specificItems"));
        }
        check(receiverGetters == 2 && methods(Receiver.class, helpers) == 4);
        check(methods(Parent.class, helpers) == 2 && methods(Child.class, helpers) == 2);
        Object token = new Object();
        Object[] values = {null, token, "value", Integer.valueOf(127), new Object[] {token}};
        int cases = 0;
        for (Receiver<Object,String> receiver : new Receiver[] {new Receiver<>(),new Child()}) {
            int events = receiver instanceof Child ? 91 : 1;
            for (Object value : values) {
                Leaf<Object> leaf = new Leaf<>();
                leaf.value = value;
                for (Leaf<Object> result : new Leaf[] {null,leaf}) {
                    receiver.value = result;
                    for (int mode = 0; mode < 5; mode++) {
                        Trace.events = 0;
                        Trace.failure = null;
                        Object actual = mode == 0 ? receiver.getItems()
                                      : mode == 1 ? ((Parent<Object>)receiver).items()
                                      : mode == 2 ? Receiver.select(receiver)
                                      : invoke(mode == 3 ? broad : narrow, receiver);
                        check(actual == result && Trace.events == events);
                        identities++;
                        cases++;
                        if (result != null) { check(result.value == value); identities++; }
                    }
                }
            }
            RuntimeException failure = new RuntimeException("owned");
            for (int mode = 0; mode < 5; mode++) {
                Trace.events = 0;
                Trace.failure = failure;
                try {
                    if (mode == 0) receiver.getItems();
                    else if (mode == 1) ((Parent<Object>)receiver).items();
                    else if (mode == 2) Receiver.select(receiver);
                    else invoke(mode == 3 ? broad : narrow,receiver);
                    throw new AssertionError();
                } catch (RuntimeException error) { check(error == failure && Trace.events == events); failures++; }
            }
        }
        Trace.failure = null;
        try { Receiver.select(null); throw new AssertionError(); }
        catch (NullPointerException expected) { failures++; }
        System.out.println("values=" + cases + ",identities=" + identities + ",failures=" + failures);
        System.out.println("distinct-final-entries-and-getter-dispatch=matched,source-name-and-bridge-flags=reported");
    }
}
