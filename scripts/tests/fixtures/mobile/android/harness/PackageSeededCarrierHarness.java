import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class PackageSeededCarrierHarness {
    static int mode, shape, failPhase;
    static boolean add;
    static Carrier input, current, suffix;
    static Object seed;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned package seeded carrier"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class Observer extends Trace {
        public void observe(int phase, Carrier owner, Object actual) {
            check(actual == (phase == 6 ? null : seed));
            if (phase == 1 || phase == 7) check(owner == input);
            else if (phase == 2 || phase == 8) suffix = owner;
            else { if (input != null) check(owner == input); current = owner; }
            events += phase + ",";
            if (phase == 3 && mode == 9) owner.values.add(seed);
            if (phase == failPhase) PackageSeededCarrierHarness.<RuntimeException>raise(failure);
            if ((phase == 2 || phase == 8) && mode == 10) Trace.alternative = new Carrier();
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return shape == 0 ? Core.call(input, seed, add) : discard();
        String name = shape == 0 ? "call" : "discard";
        if (route == 1) {
            try { return Core.class.getMethod(name, Carrier.class, Object.class, boolean.class).invoke(null, input, seed, add); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(shape == 0 ? Carrier.class : void.class, Carrier.class, Object.class, boolean.class)).invokeWithArguments(input, seed, add);
        return PackageSeededCarrierCaller.call(input, seed, add, shape);
    }
    static Object discard() { Core.discard(input, seed, add); return null; }
    static void metadata() throws Exception {
        Class<?> owner = Class.forName("fixture.Core$Factory"); check(Modifier.isPrivate(owner.getModifiers()) && Modifier.isStatic(owner.getModifiers()));
        for (String name : new String[]{"add", "remove"}) {
            Method method = owner.getDeclaredMethod(name, Carrier.class, Object.class);
            TypeVariable<?>[] formals = method.getTypeParameters(); Type[] inputs = method.getGenericParameterTypes();
            check(Modifier.isStatic(method.getModifiers()) && (method.getModifiers() & (Modifier.PUBLIC | Modifier.PRIVATE | Modifier.PROTECTED)) == 0);
            check(formals.length == 1 && formals[0].getBounds()[0] == Object.class && inputs[1].equals(formals[0]));
            check(((ParameterizedType)inputs[0]).getActualTypeArguments()[0].equals(formals[0]));
            check(((ParameterizedType)method.getGenericReturnType()).getActualTypeArguments()[0].equals(formals[0]));
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]); add = Integer.parseInt(args[3]) != 0; metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            input = mode == 1 || mode == 15 ? null : new Carrier();
            seed = mode == 2 || mode == 15 ? null : mode == 14 ? input : new Object();
            ArrayList<Object> expected = new ArrayList<Object>();
            if (input != null) {
                if (mode == 3 || mode == 11 || mode == 13) input.values.add(seed);
                else if (mode != 4) input.values.add(new Object());
                expected.addAll(input.values);
            }
            current = input; suffix = null; events = "";
            Trace.forceContainsFalse = mode == 13; Trace.emptyOverride = mode == 11 ? 0 : mode == 12 ? 1 : -1;
            Trace.alternative = new Carrier(); Trace.hook = new Observer();
            failPhase = mode == 5 ? add ? 1 : 7 : mode == 6 ? add ? 3 : 5 : mode == 7 ? add ? 4 : 6 : mode == 8 ? add ? 2 : 8 : -1;
            failure = mode == 5 || mode == 8 ? new Checked() : mode == 7 ? new LinkageError("owned failure") : new RuntimeException("owned failure");
            Object result = null; Throwable caught = null; try { result = call(route); } catch (Throwable problem) { caught = problem; }
            String wantedEvents = add ? "1," : "7,"; boolean failed = failPhase == (add ? 1 : 7); boolean nullResult = !add && input == null;
            if (!failed && add) {
                wantedEvents += "3,"; if (mode == 9) expected.add(seed); failed = failPhase == 3;
                if (!failed && (mode == 13 || !expected.contains(seed))) {
                    wantedEvents += "4,"; failed = failPhase == 4; if (!failed) expected.add(seed);
                }
                if (!failed) { wantedEvents += "2,"; failed = failPhase == 2; }
            } else if (!failed) {
                if (input != null) {
                    wantedEvents += "5,"; failed = failPhase == 5;
                    if (!failed) {
                        expected.remove(seed); wantedEvents += "6,"; failed = failPhase == 6;
                        if (!failed) nullResult = Trace.emptyOverride < 0 ? expected.isEmpty() : Trace.emptyOverride != 0;
                    }
                }
                if (!failed) { wantedEvents += "8,"; failed = failPhase == 8; }
            }
            check(events.equals(wantedEvents) && caught == (failed ? failure : null));
            if (current != null) { check(current.values.size() == expected.size()); for (int i = 0; i < expected.size(); i++) check(current.values.get(i) == expected.get(i)); }
            if (!failed) { Carrier wanted = nullResult ? null : current; check(suffix == wanted && result == (shape == 0 ? wanted : null)); }
            else check(result == null);
        }
        System.out.println("attempts=2,package-seeded-carrier=matched");
    }
}
