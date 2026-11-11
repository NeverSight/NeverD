import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class ProtectedArrayTransferHarness {
    static int shape, mode;
    static boolean derived;
    static Base subject;
    static Object[] input, initialSaved, wanted, alternative, replacement;
    static Object initialElement, written, prefixValue;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned protected array transfer"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}

    static final class Observer extends Trace {
        public void observe(int phase, Base actual, Object[] array) {
            check(actual == subject && array == input);
            events += phase + ",";
            if (phase == 1 || phase == 4) {
                check(actual.saved == initialSaved);
                if (mode == 6 || mode == 12) ProtectedArrayTransferHarness.<RuntimeException>raise(failure);
                if (mode == 10) {
                    Trace.value = prefixValue;
                    Trace.replace = true;
                    Trace.replacement = replacement;
                }
            } else if (phase == 2 || phase == 5) {
                check(actual.saved == input && input[0] == written);
                if (mode == 11) {
                    Trace.value = new Object();
                    Trace.replace = false;
                    Trace.replacement = alternative;
                }
                if (mode == 15) actual.saved = alternative;
                if (mode == 7 || mode == 13) ProtectedArrayTransferHarness.<RuntimeException>raise(failure);
            } else {
                check(phase == 3);
                if (mode == 14) ProtectedArrayTransferHarness.<RuntimeException>raise(failure);
            }
        }
    }

    static Object call(int route) throws Throwable {
        if (route == 0) return ProtectedArrayTransferCaller.call(subject, input, shape);
        String name = new String[]{"call", "object", "discard", "checked"}[shape];
        Class<?> returns = shape == 0 ? Object[].class : shape == 1 ? Object.class : shape == 2 ? void.class : String[].class;
        if (route == 1) {
            try { return Core.class.getMethod(name, Base.class, Object[].class).invoke(null, subject, input); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(returns, Base.class, Object[].class)).invokeWithArguments(subject, input);
        if (shape == 0) return Core.call(subject, input);
        if (shape == 1) return Core.object(subject, input);
        if (shape == 2) { Core.discard(subject, input); return null; }
        return Core.checked(subject, input);
    }

    static void metadata() throws Exception {
        for (Class<?> owner : new Class<?>[]{Base.class, Subject.class}) {
            Method method = owner.getDeclaredMethod("read", Object[].class);
            TypeVariable<?>[] formals = method.getTypeParameters();
            check(Modifier.isProtected(method.getModifiers()) && !Modifier.isFinal(method.getModifiers()) && !Modifier.isStatic(method.getModifiers()));
            check(formals.length == 1 && formals[0].getBounds().length == 1 && formals[0].getBounds()[0] == Object.class);
            Type result = method.getGenericReturnType();
            Type[] parameters = method.getGenericParameterTypes();
            check(result instanceof GenericArrayType && ((GenericArrayType)result).getGenericComponentType().equals(formals[0]));
            check(parameters.length == 1 && parameters[0].equals(result) && method.getReturnType() == Object[].class);
        }
    }

    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]);
        mode = Integer.parseInt(args[2]); derived = Integer.parseInt(args[3]) != 0;
        metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            Base original = derived ? new Subject() : new Base();
            initialSaved = original.saved = new Object[]{new Object()};
            subject = mode == 1 ? null : original;
            initialElement = new String("initial");
            input = mode == 2 ? null : mode == 3 ? new Object[0] : mode == 4 || mode == 5
                ? new String[]{(String)initialElement} : new Object[]{initialElement};
            prefixValue = new String("prefix");
            Trace.value = mode == 5 ? new Object() : new String("written");
            written = mode == 10 ? prefixValue : Trace.value;
            replacement = new String[]{new String("replacement")};
            alternative = new Object[]{new Object()};
            Trace.replace = mode == 8 || mode == 9 || mode == 11;
            Trace.replacement = mode == 9 ? null : replacement;
            wanted = mode == 9 ? null : mode == 8 || mode == 10 || mode == 11 ? replacement : input;
            Trace.hook = new Observer(); events = "";
            failure = mode == 6 || mode == 13 || mode == 14 ? new Checked() : new LinkageError("owned failure");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean prefixFailure = mode == 6 || mode == 12;
            boolean writeFailure = mode == 2 || mode == 3 || mode == 5;
            boolean suffixFailure = mode == 7 || mode == 13;
            boolean returned = mode != 1 && !prefixFailure && !writeFailure && !suffixFailure;
            String expected = mode == 1 ? "" : (derived ? "4," : "1,")
                + (prefixFailure || writeFailure ? "" : derived ? "5," : "2,")
                + (returned && shape == 3 ? "3," : "");
            check(events.equals(expected));
            check(original.saved == (mode == 1 || prefixFailure ? initialSaved : mode == 15 ? alternative : input));
            if (input != null && input.length != 0)
                check(input[0] == (mode == 1 || prefixFailure || writeFailure ? initialElement : written));
            check(replacement.length == 1 && replacement[0] instanceof String && replacement[0].equals("replacement"));
            if (mode == 1 || mode == 2) check(caught instanceof NullPointerException);
            else if (mode == 3) check(caught instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 5) check(caught instanceof ArrayStoreException);
            else if (prefixFailure || suffixFailure || mode == 14 && shape == 3) check(caught == failure);
            else if (shape == 3 && wanted != null && !(wanted instanceof String[])) check(caught instanceof ClassCastException);
            else check(caught == null && result == (shape == 2 ? null : wanted));
        }
        System.out.println("attempts=2,protected-array-transfer=matched");
    }
}
