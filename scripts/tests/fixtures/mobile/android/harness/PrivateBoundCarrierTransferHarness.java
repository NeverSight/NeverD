import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class PrivateBoundCarrierTransferHarness {
    static int shape, mode;
    static Core subject;
    static Slot<?> input, initialSaved, wanted, alternative, replacement;
    static Object initialElement, written, prefixValue, replacementElement;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned private bound carrier transfer"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}

    static final class Other<E> implements Slot<E> {
        E value;
        public void put(E value) {
            Trace.hit(4, null, this);
            this.value = value;
            Trace.hit(5, null, this);
        }
    }

    static Object element(Slot<?> slot) {
        return slot instanceof Bag ? ((Bag<?>)slot).value : ((Other<?>)slot).value;
    }

    static final class Observer extends Trace {
        public void observe(int phase, Core actual, Slot<?> slot) {
            check(slot == input);
            check(actual == (phase == 4 || phase == 5 ? null : subject));
            events += phase + ",";
            if (phase == 1) {
                check(actual.saved == initialSaved);
                if (mode == 6 || mode == 12) PrivateBoundCarrierTransferHarness.<RuntimeException>raise(failure);
                if (mode == 10) {
                    Trace.value = prefixValue;
                    Trace.replace = true;
                    Trace.replacement = replacement;
                }
            } else if (phase == 4) {
                check(subject.saved == input && element(input) == initialElement);
                if (mode == 4) PrivateBoundCarrierTransferHarness.<RuntimeException>raise(failure);
            } else if (phase == 5) {
                check(subject.saved == input && element(input) == written);
                if (mode == 5) PrivateBoundCarrierTransferHarness.<RuntimeException>raise(failure);
            } else if (phase == 2) {
                check(actual.saved == input && element(input) == written);
                if (mode == 11) {
                    Trace.value = new Object();
                    Trace.replace = false;
                    Trace.replacement = alternative;
                }
                if (mode == 15) actual.saved = alternative;
                if (mode == 7 || mode == 13) PrivateBoundCarrierTransferHarness.<RuntimeException>raise(failure);
            } else {
                check(phase == 3);
                if (mode == 14) PrivateBoundCarrierTransferHarness.<RuntimeException>raise(failure);
            }
        }
    }

    static Object call(int route) throws Throwable {
        if (route == 0) return PrivateBoundCarrierTransferCaller.call(subject, input, shape);
        String name = new String[]{"call", "object", "discard", "checked"}[shape];
        Class<?> returns = shape == 0 ? Slot.class : shape == 1 ? Object.class : shape == 2 ? void.class : Bag.class;
        if (route == 1) {
            try { return Core.class.getMethod(name, Core.class, Slot.class).invoke(null, subject, input); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(returns, Core.class, Slot.class)).invokeWithArguments(subject, input);
        if (shape == 0) return Core.call(subject, input);
        if (shape == 1) return Core.object(subject, input);
        if (shape == 2) { Core.discard(subject, input); return null; }
        return Core.checked(subject, input);
    }

    static void metadata() throws Exception {
        Method method = Core.class.getDeclaredMethod("read", Slot.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        check(Modifier.isPrivate(method.getModifiers()) && !Modifier.isStatic(method.getModifiers()) && !method.isBridge() && !method.isSynthetic());
        check(formals.length == 2 && formals[0].getBounds().length == 1 && formals[0].getBounds()[0] == Object.class);
        check(formals[1].getBounds().length == 1 && formals[1].getBounds()[0] instanceof ParameterizedType);
        ParameterizedType bound = (ParameterizedType)formals[1].getBounds()[0];
        check(bound.getRawType() == Slot.class && bound.getActualTypeArguments().length == 1 && bound.getActualTypeArguments()[0].equals(formals[0]));
        check(method.getGenericParameterTypes().length == 1 && method.getGenericParameterTypes()[0].equals(formals[1]));
        check(method.getGenericReturnType().equals(formals[1]) && method.getReturnType() == Slot.class && method.getExceptionTypes().length == 0);
        Method checked = Core.class.getMethod("checked", Core.class, Slot.class);
        TypeVariable<?> value = checked.getTypeParameters()[0];
        ParameterizedType result = (ParameterizedType)checked.getGenericReturnType();
        check(result.getRawType() == Bag.class && result.getActualTypeArguments()[0].equals(value));
    }

    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]);
        mode = Integer.parseInt(args[2]); boolean other = Integer.parseInt(args[3]) != 0;
        metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            Core original = new Core(); initialSaved = original.saved = new Bag<Object>();
            subject = mode == 1 ? null : original;
            initialElement = new Object();
            Bag<Object> bag = new Bag<Object>(); bag.value = initialElement;
            Other<Object> different = new Other<Object>(); different.value = initialElement;
            input = mode == 2 ? null : other ? different : bag;
            prefixValue = new Object();
            Trace.value = mode == 3 ? null : new Object();
            written = mode == 10 ? prefixValue : Trace.value;
            Bag<Object> replacementBag = new Bag<Object>();
            replacementElement = replacementBag.value = new Object();
            replacement = replacementBag;
            alternative = new Other<Object>();
            Trace.replace = mode == 8 || mode == 9 || mode == 11;
            Trace.replacement = mode == 9 ? null : replacement;
            wanted = mode == 9 ? null : mode == 8 || mode == 10 || mode == 11 ? replacement : input;
            Trace.hook = new Observer(); events = "";
            failure = mode == 4 || mode == 6 || mode == 13 || mode == 14 ? new Checked() : new LinkageError("owned failure");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean prefixFailure = mode == 6 || mode == 12;
            boolean beforeWrite = mode == 1 || mode == 2 || prefixFailure || mode == 4;
            boolean putFailure = mode == 4 || mode == 5;
            boolean suffixFailure = mode == 7 || mode == 13;
            boolean returned = mode != 1 && mode != 2 && !prefixFailure && !putFailure && !suffixFailure;
            String expected = mode == 1 ? "" : "1,"
                + (prefixFailure || mode == 2 ? "" : "4," + (mode == 4 ? "" : "5," + (mode == 5 ? "" : "2,")))
                + (returned && shape == 3 ? "3," : "");
            check(events.equals(expected));
            check(original.saved == (mode == 1 || prefixFailure ? initialSaved : mode == 15 ? alternative : input));
            if (input != null) check(element(input) == (beforeWrite ? initialElement : written));
            check(element(replacement) == replacementElement);
            if (mode == 1 || mode == 2) check(caught instanceof NullPointerException);
            else if (prefixFailure || putFailure || suffixFailure || mode == 14 && shape == 3) check(caught == failure);
            else if (shape == 3 && wanted != null && !(wanted instanceof Bag)) check(caught instanceof ClassCastException);
            else check(caught == null && result == (shape == 2 ? null : wanted));
        }
        System.out.println("attempts=2,private-bound-carrier-transfer=matched");
    }
}
