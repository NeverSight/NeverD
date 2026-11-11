import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class BoundedArrayContextVoidHarness {
    static int mode;
    static Object owner, name, input;
    static Context context;
    static String key;
    static Mark[] values;
    static Object offered, changed;
    static int index;
    static String events;
    static Throwable failure;
    static int beforeCalls;
    static void check(boolean value) { if (!value) throw new AssertionError("owned bounded array context void"); }
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E)failure; }
    static final class Checked extends Exception {}
    static final class Other implements Mark {}
    static final class FutureContext extends Context {
        public void before(String name, Mark[] array) {
            beforeCalls++;
            check(this == context && name == key && array == values && events.equals("0,1,"));
            if (mode == 17) events += "B,";
            if (mode == 16) { next = changed; this.index = 1; }
            if (mode == 10 || mode == 23) BoundedArrayContextVoidHarness.<RuntimeException>raise(failure);
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Context target, String name, Object array, Object item, int count) {
            events += phase + ",";
            check(target == context && name == key && array == values);
            if (phase == 2 || phase == 3) {
                check(item == (mode == 16 ? changed : offered) && count == index);
                if (phase == 2 && mode == 15) { context.next = changed; context.index = 1; }
            } else check(item == null && count == 0);
            if (phase == (mode == 9 ? 1 : mode == 11 || mode == 12 ? 2 : mode == 13 ? 3 : mode == 14 ? 4 : mode == 19 ? 0 : -1))
                BoundedArrayContextVoidHarness.<RuntimeException>raise(failure);
        }
    }
    static void call(int route) throws Throwable {
        if (route == 0) Core.run(owner, name, input);
        else if (route == 1) Core.typed(owner, name, input);
        else if (route == 2) {
            try { Core.class.getMethod("run", Object.class, Object.class, Object.class).invoke(null, owner, name, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else MethodHandles.publicLookup().findStatic(Core.class, "run",
            MethodType.methodType(void.class, Object.class, Object.class, Object.class)).invokeExact(owner, name, input);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        Method method = Provider.class.getMethod("write", Context.class, String.class, Mark[].class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        check(Modifier.isStatic(method.getModifiers()) && formals.length == 1 && method.getReturnType() == void.class);
        check(formals[0].getBounds().length == 1 && formals[0].getBounds()[0] == Mark.class);
        check(((GenericArrayType)method.getGenericParameterTypes()[2]).getGenericComponentType().equals(formals[0]));
        for (int attempt = 0; attempt < 2; attempt++) {
            events = ""; beforeCalls = 0; Trace.hook = new Observer();
            Context original = new FutureContext(); context = mode == 2 ? null : original;
            key = mode == 20 ? null : new String("owned key");
            values = mode == 3 ? null : mode == 4 ? new Mark[0] : mode == 8 ? new Leaf[] {new Leaf(), new Leaf()} : new Mark[] {new Leaf(), new Leaf()};
            Object[] before = values == null ? null : values.clone();
            offered = mode == 6 ? new Object() : mode == 7 ? null : mode == 8 ? new Other() : new Leaf();
            changed = new Other(); index = mode == 1 || mode == 16 ? 1 : mode == 5 ? -1 : 0;
            original.index = mode == 16 ? 0 : index; original.next = offered;
            owner = mode == 22 ? new Object() : context; name = mode == 21 ? new Object() : key;
            input = mode == 18 ? new Object[] {new Object()} : values;
            failure = mode == 11 || mode == 13 || mode == 19 ? new Checked() : mode == 12 || mode == 14 || mode == 23 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Throwable caught = null;
            try { call(route); } catch (Throwable problem) { caught = problem; }
            String expected = mode == 18 || mode == 21 || mode == 22 ? "" : "0,";
            if (!expected.isEmpty() && mode != 19) expected += "1,";
            if (!expected.isEmpty() && mode != 19 && mode != 2 && mode != 9 && mode != 10 && mode != 23) {
                if (mode == 17) expected += "B,";
                if (mode != 6) {
                    expected += "2,";
                    if (mode != 3 && mode != 4 && mode != 5 && mode != 8 && mode != 11 && mode != 12) {
                        expected += "3,";
                        if (mode != 13) expected += "4,";
                    }
                }
            }
            check(events.equals(expected));
            check(beforeCalls == (mode == 18 || mode == 21 || mode == 22 || mode == 19 || mode == 2 || mode == 9 ? 0 : 1));
            if (mode == 9 || mode == 10 || mode == 11 || mode == 12 || mode == 13 || mode == 14 || mode == 19 || mode == 23) check(caught == failure);
            else if (mode == 2 || mode == 3) check(caught != null && caught.getClass() == NullPointerException.class);
            else if (mode == 4 || mode == 5) check(caught != null && caught.getClass() == ArrayIndexOutOfBoundsException.class);
            else if (mode == 6 || mode == 18 || mode == 21 || mode == 22) check(caught != null && caught.getClass() == ClassCastException.class);
            else if (mode == 8) check(caught != null && caught.getClass() == ArrayStoreException.class);
            else check(caught == null);
            boolean stored = expected.contains("3,");
            if (values != null) for (int i = 0; i < values.length; i++)
                check(values[i] == (stored && i == index ? mode == 16 ? changed : offered : before[i]));
        }
        System.out.println("attempts=2,bounded-array-context-void=matched");
    }
}
