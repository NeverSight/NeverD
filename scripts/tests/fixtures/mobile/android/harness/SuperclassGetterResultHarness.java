import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class SuperclassGetterResultHarness {
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static Object invoke(Method method, Object receiver) throws Throwable {
        try { return Modifier.isStatic(method.getModifiers()) ? method.invoke(null,receiver) : method.invoke(receiver); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean original = args[0].equals("original");
        Method getter = Adapter.class.getDeclaredMethod("getKeys");
        Method child = View.class.getDeclaredMethod("getKeys");
        Method wrapper = Adapter.class.getDeclaredMethod("keySet");
        check(getter.isBridge() == original && wrapper.isBridge() == original);
        check(!child.isBridge() && !getter.isSynthetic() && !wrapper.isSynthetic());
        check(!Modifier.isFinal(getter.getModifiers()) && Modifier.isFinal(wrapper.getModifiers()));
        check(getter.getGenericReturnType().getTypeName().equals(original ? "java.util.Set<java.lang.Object>" : "java.util.Set<T>"));
        check(wrapper.getGenericReturnType().getTypeName().equals("java.util.Set<T>"));
        check(child.getGenericReturnType().getTypeName().equals("java.util.Set<A>"));
        check(Adapter.class.getGenericSuperclass().getTypeName().equals("java.util.AbstractMap<T, U>"));
        check(View.class.getGenericSuperclass().getTypeName().equals("fixture.Adapter<A, B>"));
        View<Object,String> view = new View<>();
        Object token = new Object();
        Set<Object>[] values = new Set[] {null,Collections.emptySet(),Collections.singleton(null),
            Collections.singleton(token),Collections.singleton("value"),Collections.singleton(Integer.valueOf(123)),
            new LinkedHashSet<>(Arrays.asList(token,"value",null))};
        int cases = 0, identities = 0, failures = 0;
        for (Set<Object> value : values) {
            view.value = value;
            for (int mode = 0; mode < 5; mode++) {
                Trace.events = 0;
                Trace.failure = null;
                Object actual = mode == 0 ? view.keySet()
                              : mode == 1 ? ((Map<Object,String>)view).keySet()
                              : mode == 2 ? View.select(view)
                              : invoke(mode == 3 ? getter : child,view);
                check(actual == value && Trace.events == 1);
                cases++;
                identities++;
            }
        }
        RuntimeException failure = new RuntimeException("owned");
        for (int mode = 0; mode < 5; mode++) {
            Trace.events = 0;
            Trace.failure = failure;
            try {
                if (mode == 0) view.keySet();
                else if (mode == 1) ((Map<Object,String>)view).keySet();
                else if (mode == 2) View.select(view);
                else invoke(mode == 3 ? getter : child, view);
                throw new AssertionError();
            } catch (RuntimeException error) { check(error == failure && Trace.events == 1); failures++; }
        }
        Trace.failure = null;
        Trace.events = 0;
        Set<Object> parent = view.originalSuperKeys();
        check(parent != null && view.originalSuperKeys() == parent && Trace.events == 0);
        identities++;
        try { View.select(null); throw new AssertionError(); }
        catch (NullPointerException expected) { failures++; }
        if (args[1].equals("integer")) {
            Method scalar = Adapter.class.getDeclaredMethod("getSize");
            Method own = View.class.getDeclaredMethod("getSize");
            Method entry = Adapter.class.getDeclaredMethod("size");
            check(scalar.isBridge() == original && entry.isBridge() == original && !own.isBridge());
            check(!scalar.isSynthetic() && !entry.isSynthetic() && Modifier.isFinal(entry.getModifiers()));
            int[] numbers = {Integer.MIN_VALUE,-1024,-1,0,1,513,Integer.MAX_VALUE};
            for (int number : numbers) {
                View.class.getField("scalar").setInt(view,number);
                for (int mode = 0; mode < 5; mode++) {
                    Trace.events = 0;
                    Object actual = mode == 0 ? Integer.valueOf(view.size())
                                  : mode == 1 ? Integer.valueOf(((Map<Object,String>)view).size())
                                  : mode == 2 ? View.class.getMethod("selectSize",View.class).invoke(null,view)
                                  : invoke(mode == 3 ? scalar : own,view);
                    check(Integer.valueOf(number).equals(actual) && Trace.events == 2);
                    cases++;
                }
            }
            for (Method method : new Method[] {entry,Map.class.getMethod("size"),
                                               View.class.getMethod("selectSize",View.class),scalar,own}) {
                Trace.events = 0;
                Trace.failure = failure;
                try { invoke(method,view); throw new AssertionError(); }
                catch (RuntimeException error) { check(error == failure && Trace.events == 2); failures++; }
            }
            Trace.failure = null;
            Trace.events = 0;
            try { invoke(View.class.getMethod("originalSuperSize"),view); throw new AssertionError(); }
            catch (NullPointerException expected) { check(Trace.events == 0); failures++; }
            try {
                View.class.getMethod("selectSize",View.class).invoke(null,new Object[] {null});
                throw new AssertionError();
            } catch (InvocationTargetException error) { check(error.getCause() instanceof NullPointerException); failures++; }
        }
        System.out.println("values=" + cases + ",identities=" + identities + ",failures=" + failures);
        System.out.println("original-super-body-and-virtual-getter=matched,broad-result-projection-and-flags=reported");
    }
}
