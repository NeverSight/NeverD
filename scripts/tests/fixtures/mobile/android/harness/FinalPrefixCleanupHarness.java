import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class FinalPrefixCleanupHarness {
    static int mode, failPhase, completed;
    static boolean choice;
    static Context context;
    static Object a, b, first, second;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned final prefix cleanup"); }
    static final class Checked extends Exception {}
    static final class Observer extends Trace {
        public void observe(int phase, Core owner, Object value) throws Throwable {
            check(owner == Trace.leaked);
            check(owner.first == (phase >= 2 ? FinalPrefixCleanupHarness.first : null));
            check(owner.second == (phase >= 3 ? FinalPrefixCleanupHarness.second : null));
            check(value == (phase == 1 ? a : phase == 2 ? b : null));
            events += phase + ",";
            if (phase == failPhase) throw failure;
            completed = Math.max(completed, phase);
            if (phase == 6 && mode == 15) { Trace.first = new Object(); Trace.second = new Object(); }
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return new Core(context, choice, a, b);
        if (route == 1) {
            try { return Core.class.getConstructor(Context.class, boolean.class, Object.class, Object.class).newInstance(context, choice, a, b); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findConstructor(Core.class,
            MethodType.methodType(void.class, Context.class, boolean.class, Object.class, Object.class))
            .invokeWithArguments(context, choice, a, b);
        return FinalPrefixCleanupCaller.make(context, choice, a, b);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); choice = Integer.parseInt(args[1]) != 0; mode = Integer.parseInt(args[2]);
        check(Modifier.isFinal(Core.class.getField("first").getModifiers()) && Modifier.isFinal(Core.class.getField("second").getModifiers()));
        for (int attempt = 0; attempt < 2; attempt++) {
            first = mode == 12 ? null : new Object(); second = mode == 13 ? null : mode == 10 ? first : new Object();
            a = mode == 1 ? null : mode == 11 ? new Object() : first; b = mode == 11 ? new Object() : second;
            context = mode == 9 ? null : new Context(); failPhase = mode >= 2 && mode <= 8 ? mode - 1 : -1;
            failure = (mode & 1) == 0 ? new Checked() : new LinkageError("owned failure");
            Trace.first = first; Trace.second = second; Trace.forceReject = mode == 14; Trace.rejection = new RuntimeException("owned rejection");
            Trace.leaked = null; Trace.hook = new Observer(); events = ""; completed = -1;
            Object result = null; Throwable caught = null; try { result = call(route); } catch (Throwable problem) { caught = problem; }
            String expected = ""; Throwable wanted = null;
            if (wanted == null) {
                if (context == null) wanted = caught;
                else {
                    expected += "1,"; if (failPhase == 1) wanted = failure;
                    if (wanted == null) { expected += "2,"; if (failPhase == 2) wanted = failure; }
                    if (wanted == null) {
                        expected += "3,"; if (failPhase == 3) wanted = failure;
                        if (wanted == null) { expected += "4,"; if (failPhase == 4) wanted = failure; }
                        if (wanted == null) {
                            if (!choice || mode == 14) wanted = Trace.rejection;
                            else { expected += "5,"; if (failPhase == 5) wanted = failure; }
                        }
                        expected += "6,"; if (failPhase == 6) wanted = failure;
                        if (wanted == null) { expected += "7,"; if (failPhase == 7) wanted = failure; }
                    }
                }
            }
            check(events.equals(expected));
            check(caught == wanted && (mode != 9 || caught instanceof NullPointerException));
            check(Trace.leaked != null && Trace.leaked.first == (completed >= 1 ? first : null) && Trace.leaked.second == (completed >= 2 ? second : null));
            check(wanted == null ? result == Trace.leaked : result == null);
        }
        System.out.println("attempts=2,final-prefix-cleanup=matched");
    }
}
