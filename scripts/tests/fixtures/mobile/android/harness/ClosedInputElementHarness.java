import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class ClosedInputElementHarness {
    static int shape, mode, attempt, calls, stores;
    static Subject receiver;
    static Class token;
    static Set<Class<?>> supplied, actual;
    static Object wanted;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned closed input element"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class Second implements Factory<Object> {
        public Object create(Set<Class<?>> seen) { throw new AssertionError("type token was executed"); }
    }
    static final class Probe implements Factory<Object> {
        public Object create(Set<Class<?>> seen) {
            calls++; events += "C,"; check(seen == actual && seen.contains(token));
            if (mode == 4 || mode == 5) ClosedInputElementHarness.<RuntimeException>raise(failure);
            return wanted;
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Subject target, Class<?> current, Set<Class<?>> seen) {
            events += phase + ","; check(target == receiver);
            if (shape == 2) {
                if (actual == null) actual = seen;
                check(seen == actual && seen != supplied);
            } else check(seen == supplied);
            if (phase == 1) {
                check(current == token || mode == 11 && current == Second.class);
                if (mode == 3) ClosedInputElementHarness.<RuntimeException>raise(failure);
                if (mode == 13) Trace.factory = new Probe();
            } else {
                stores++;
                check(target.state == 7 + stores && target.result == wanted);
                if (mode == 12) target.result = new Object();
                if (mode == 6 || mode == 15) ClosedInputElementHarness.<RuntimeException>raise(failure);
            }
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return Core.call(receiver, token, supplied, shape);
        if (route == 1) {
            try { return Core.class.getMethod("call", Subject.class, Class.class, Set.class, int.class)
                .invoke(null, receiver, token, supplied, shape); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findStatic(Core.class, "call",
            MethodType.methodType(Object.class, Subject.class, Class.class, Set.class, int.class))
            .invokeWithArguments(receiver, token, supplied, shape);
        return ClosedInputElementCaller.call(receiver, token, supplied, shape);
    }
    static void metadata() throws Exception {
        for (String name : new String[]{"select", "entry"}) {
            Method method = name.equals("select") ? Subject.class.getDeclaredMethod(name, Class.class, Set.class)
                : Subject.class.getDeclaredMethod(name, Class.class);
            TypeVariable<?>[] formals = method.getTypeParameters();
            check(formals.length == 1 && formals[0].getBounds()[0] == Object.class);
            check(method.getGenericReturnType().equals(formals[0]) && method.getReturnType() == Object.class);
            check(!Modifier.isStatic(method.getModifiers()) && (name.equals("select") ? Modifier.isPrivate(method.getModifiers())
                : (method.getModifiers() & (Modifier.PUBLIC | Modifier.PROTECTED | Modifier.PRIVATE)) == 0));
            Type arg = ((ParameterizedType)method.getGenericParameterTypes()[0]).getActualTypeArguments()[0];
            check(arg instanceof WildcardType && ((WildcardType)arg).getLowerBounds().length == 0);
            check(((ParameterizedType)((WildcardType)arg).getUpperBounds()[0]).getRawType() == Factory.class);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]); metadata();
        for (attempt = 0; attempt < 2; attempt++) {
            Subject original = new Subject(); original.state = 7;
            Object initial = new String("initial " + attempt); original.result = initial;
            receiver = mode == 7 ? null : original; token = mode == 9 ? null : mode == 14 ? Integer.class : Probe.class;
            supplied = mode == 8 ? null : new HashSet<Class<?>>(); actual = shape == 2 ? null : supplied;
            if (mode == 10 && supplied != null) supplied.add(token);
            wanted = mode == 2 ? new Object() : new String("result " + attempt);
            Trace.value = wanted; Trace.factory = mode == 1 || mode == 13 ? null : new Probe();
            Trace.next = mode == 11 ? Second.class : null; Trace.hook = new Observer();
            events = ""; calls = stores = 0;
            failure = mode == 3 || mode == 6 ? new Checked() : mode == 5 || mode == 15 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean entered = mode != 7;
            boolean duplicate = mode == 10 && shape != 2;
            boolean badSet = mode == 8 && shape != 2;
            boolean provider = entered && !duplicate && !badSet && mode != 3 && mode != 1;
            boolean stored = provider && mode != 4 && mode != 5;
            int count = mode == 11 ? 2 : 1;
            String expected = entered ? "1," : "";
            if (provider && mode == 11) expected += "1,";
            if (provider) expected += stored ? (mode == 11 ? "C,2,C,2," : "C,2,") : "C,";
            check(events.equals(expected) && calls == (provider ? count : 0) && stores == (stored ? count : 0));
            check(original.state == 7 + stores && original.result == (stored ? mode == 12 ? original.result : wanted : initial));
            if (stored && mode == 12) check(original.result != wanted);
            if (mode == 3 || mode == 4 || mode == 5 || mode == 6 || mode == 15) check(caught == failure);
            else if (mode == 7 || badSet || mode == 1) check(caught instanceof NullPointerException);
            else if (mode == 2 && shape == 1) check(caught instanceof ClassCastException && stores == 1);
            else check(caught == null && result == (shape == 3 ? null : duplicate ? initial : wanted));
            if (entered && mode != 3 && !badSet) check(actual.contains(token));
        }
        System.out.println("attempts=2,closed-input-element=matched");
    }
}
