import fixture.*;
import java.util.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class BoundedArrayTokenHarness {
    static int events, mode;
    static boolean discard, carrier;
    static Context context;
    static String key;
    static Token token;
    static Object selected;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned bounded array token"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class Other implements Mark {}
    static Object replacement(boolean other) {
        Mark value = other ? new Other() : new Leaf();
        return carrier ? Arrays.asList(value) : other ? new Mark[] {value} : new Leaf[] {(Leaf)value};
    }
    static final class FutureContext extends Context {
        int calls;
        Throwable failure;
        public void before(String name, Token<?> type) {
            calls++;
            check(this == context && name == key && type == token && events == 31);
            if (mode == 13 || mode == 15) reply = selected = replacement(false);
            if (mode == 4 || mode == 5) BoundedArrayTokenHarness.<RuntimeException>raise(failure);
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Context owner, String name, Token<?> type, Object result) {
            events = events * 10 + phase;
            check(owner == context && name == key && type == token);
            if (phase == 2) {
                check(result == selected);
                if (mode == 14) context.reply = replacement(true);
            } else if (phase == 4) check(result == (discard ? null : selected));
            else check(result == null);
            if (phase == (mode == 6 ? 1 : mode == 7 ? 2 : mode == 8 ? 3 : mode == 9 ? 4 : 0))
                BoundedArrayTokenHarness.<RuntimeException>raise(failure);
        }
    }
    static Object call(int route, boolean ignored) throws Throwable {
        if (route == 0) {
            if (!ignored) return Core.run(context, key, token);
            Core.ignore(context, key, token); return null;
        }
        String method = ignored ? "ignore" : "run";
        if (route == 1) {
            try { return Core.class.getMethod(method, Object.class, Object.class, Object.class).invoke(null, context, key, token); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle target = MethodHandles.publicLookup().findStatic(Core.class, method,
            MethodType.methodType(ignored ? void.class : carrier ? List.class : Leaf[].class, Object.class, Object.class, Object.class));
        return target.invokeWithArguments(context, key, token);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        discard = Boolean.parseBoolean(args[2]);
        carrier = args.length > 3 && Boolean.parseBoolean(args[3]);
        TypeVariable<?> parameter = Provider.class.getMethod("read", Context.class, String.class, Token.class).getTypeParameters()[0];
        check(parameter.getBounds().length == 1 && parameter.getBounds()[0] == Mark.class);
        for (int attempt = 0; attempt < 2; attempt++) {
            events = 0; Trace.hook = new Observer();
            FutureContext original = new FutureContext(); context = mode == 10 ? null : original;
            key = mode == 11 ? null : new String("owned key"); token = mode == 12 ? null : new Token();
            failure = mode == 7 ? new AssertionError("owned trace error") : mode == 9 ? new Checked() : new RuntimeException("owned trace failure");
            original.failure = mode == 5 ? new AssertionError("owned callback error") : new RuntimeException("owned callback failure");
            selected = mode == 1 ? replacement(true) : mode == 2 ? new Object[] {new Object()} :
                mode == 3 ? null : replacement(false);
            original.reply = selected;
            Object answer = null; Throwable caught = null;
            try { answer = call(route, discard); } catch (Throwable problem) { caught = problem; }
            int expected = mode == 8 ? 3 : mode == 6 || mode == 4 || mode == 5 || mode == 10 || mode == 2 ? 31 : mode == 7 ? 312 : 3124;
            check(events == expected);
            check(original.calls == (mode == 8 || mode == 6 || mode == 10 ? 0 : 1));
            if (mode >= 6 && mode <= 9) check(caught == failure);
            else if (mode == 4 || mode == 5) check(caught == original.failure);
            else if (mode == 10) check(caught instanceof NullPointerException);
            else if (mode == 2 || mode == 1 && !discard && !carrier) check(caught instanceof ClassCastException);
            else {
                check(caught == null && answer == (discard ? null : selected));
                if (carrier && !discard && answer != null) {
                    try { Leaf first = (Leaf)((List)answer).get(0); check(mode != 1 && first instanceof Leaf); }
                    catch (ClassCastException expectedCast) { check(mode == 1 && events == 3124); }
                }
            }
        }
        System.out.println("attempts=2,bounded-array-token=matched");
    }
}
