package fixture;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class DeferredFieldStringHarness {
    static final String LABEL = "owned allocation label";
    static int mode, shape, attempt, initializations, constructors;
    static Owner owner, sentinel;
    static Object captured, afterInitialization, afterConstructor;
    static Throwable failure;
    static String events;
    static void check(boolean value) { if (!value) throw new AssertionError("owned deferred field string mismatch"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class Observer extends Trace {
        public void observe(int phase, Target target) {
            events += phase + ",";
            if (phase == 1) {
                initializations++; check(target == null);
                if (mode == 3 || mode == 4 || mode == 15) DeferredFieldStringHarness.<RuntimeException>raise(failure);
                if (owner != null) owner.value = afterInitialization;
                Owner.shared = afterInitialization;
                captured = afterInitialization;
            } else {
                constructors++;
                check(target.text == LABEL && target.marker == null && !target.flag && target.value == captured);
                Trace.last = target;
                if (owner != null) owner.value = afterConstructor;
                Owner.shared = afterConstructor;
                if (mode == 10) target.text = new String(LABEL);
                if (mode == 11) target.flag = true;
                if (mode == 12) target.value = afterConstructor;
                if (mode == 13) Trace.lastOwner = new Owner();
                if (mode == 5 || mode == 6 || mode == 14) DeferredFieldStringHarness.<RuntimeException>raise(failure);
            }
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return shape == 0 ? Core.fromInstance(owner) : Core.fromStatic();
        if (route == 1) return DeferredFieldStringCaller.call(shape, owner);
        String name = shape == 0 ? "fromInstance" : "fromStatic";
        Class<?>[] types = shape == 0 ? new Class<?>[]{Owner.class} : new Class<?>[]{};
        Object[] arguments = shape == 0 ? new Object[]{owner} : new Object[]{};
        if (route == 2) {
            try { return Core.class.getMethod(name, types).invoke(null, arguments); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        return MethodHandles.publicLookup().findStatic(Core.class, name, MethodType.methodType(Target.class, types)).invokeWithArguments(arguments);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]);
        check(Modifier.isVolatile(Owner.class.getField("value").getModifiers()));
        check(Modifier.isVolatile(Owner.class.getField("shared").getModifiers()));
        check(Target.class.getConstructor(String.class,Object.class,boolean.class,Object.class).getParameterTypes().length == 4);
        for (attempt = 0; attempt < 2; attempt++) {
            Owner original = new Owner(); sentinel = new Owner();
            owner = mode == 1 || mode == 15 ? null : original;
            captured = mode == 2 ? null : mode == 7 ? new String("opaque value") : mode == 8 ? original : new Object();
            afterInitialization = mode == 2 ? null : mode == 8 ? original : new Object();
            afterConstructor = new Object(); original.value = captured; Owner.shared = captured;
            Trace.lastOwner = sentinel; Trace.last = null; Trace.hook = new Observer();
            events = ""; initializations = constructors = 0;
            failure = mode == 3 || mode == 5 ? new Checked() : mode == 4 || mode == 6 || mode == 15 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean failedInitialization = mode == 3 || mode == 4 || mode == 15;
            boolean nullReceiver = shape == 0 && owner == null;
            boolean constructed = !failedInitialization && !nullReceiver;
            boolean failedConstructor = mode == 5 || mode == 6 || mode == 14;
            check(initializations == (attempt == 0 ? 1 : 0) && constructors == (constructed ? 1 : 0));
            check(events.equals((attempt == 0 ? "1," : "") + (constructed ? "2," : "")));
            if (failedInitialization) {
                if (attempt != 0) check(caught instanceof NoClassDefFoundError);
                else if (mode == 4 || mode == 15) check(caught == failure);
                else check(caught instanceof ExceptionInInitializerError && caught.getCause() == failure);
                check(Trace.last == null && result == null && Trace.lastOwner == sentinel);
            } else if (nullReceiver) check(caught instanceof NullPointerException && Trace.last == null && result == null && Trace.lastOwner == sentinel);
            else {
                Target target = Trace.last;
                check(target != null && target.marker == null && target.flag == (mode == 11));
                check(target.value == (mode == 12 ? afterConstructor : captured));
                check(mode == 10 ? target.text != LABEL && target.text.equals(LABEL) : target.text == LABEL);
                if (failedConstructor) check(caught == failure && result == null && Trace.lastOwner == sentinel);
                else {
                    check(caught == null && result == target);
                    if (shape == 0) check(Trace.lastOwner == owner);
                }
            }
        }
        System.out.println("attempts=2,deferred-field-string=matched");
    }
}
