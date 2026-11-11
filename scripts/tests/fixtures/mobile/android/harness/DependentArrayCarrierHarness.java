import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class DependentArrayCarrierHarness {
    static int mode, tokenCalls, inputCalls, resultCalls;
    static boolean checked;
    static Object type, value, tokenValue, inputValue, chosen, replacement;
    static Token<?> token;
    static Carrier<?> input;
    static String events, text;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned dependent array carrier"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class TokenProbe implements Token<Object> {
        public Object read() {
            tokenCalls++;
            if (mode == 6) DependentArrayCarrierHarness.<RuntimeException>raise(failure);
            if (mode == 17) Trace.output = chosen = replacement;
            return tokenValue;
        }
    }
    static final class InputProbe implements Carrier<Object> {
        public Object read() {
            inputCalls++;
            if (mode == 8) DependentArrayCarrierHarness.<RuntimeException>raise(failure);
            if (mode == 18) Trace.output = chosen = replacement;
            return inputValue;
        }
    }
    static final class ResultProbe implements Carrier<Object> {
        final Object payload;
        ResultProbe(Object payload) { this.payload = payload; }
        public Object read() {
            resultCalls++;
            if (mode == 20) DependentArrayCarrierHarness.<RuntimeException>raise(failure);
            return payload;
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Object type, Object value, Object item, Object result) {
            events += phase + ",";
            check(type == token && value == input);
            check(item == (phase == 2 ? tokenValue : phase == 3 || phase == 4 ? inputValue : null));
            check(result == (phase == 4 || phase == 5 ? chosen : null));
            if (phase == 4 && mode == 19) Trace.output = replacement;
            if (phase == (mode == 5 ? 1 : mode == 7 ? 2 : mode == 9 ? 3 : mode == 11 ? 4 : mode == 12 ? 5 : -1))
                DependentArrayCarrierHarness.<RuntimeException>raise(failure);
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return checked ? Core.checked(type, value) : Core.run(type, value);
        if (route == 1) return checked ? DependentArrayCarrierCaller.checked(type, value) : DependentArrayCarrierCaller.run(type, value);
        String name = checked ? "checked" : "run";
        if (route == 2) {
            try { return Core.class.getMethod(name, Object.class, Object.class).invoke(null, type, value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(checked ? String.class : Object.class, Object.class, Object.class)).invokeWithArguments(type, value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); checked = Boolean.parseBoolean(args[1]); mode = Integer.parseInt(args[2]);
        Method method = Provider.class.getMethod("make", Token.class, Carrier.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        check(formals.length == 2 && formals[0].getBounds().length == 1 && formals[0].getBounds()[0] == Object.class);
        check(formals[1].getBounds().length == 1 && formals[1].getBounds()[0].equals(formals[0]));
        check(((ParameterizedType)method.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(formals[0]));
        check(((ParameterizedType)method.getGenericParameterTypes()[1]).getActualTypeArguments()[0].equals(formals[1]));
        Type array = ((ParameterizedType)method.getGenericReturnType()).getActualTypeArguments()[0];
        check(array instanceof GenericArrayType && ((GenericArrayType)array).getGenericComponentType().equals(formals[1]));
        for (int attempt = 0; attempt < 2; attempt++) {
            events = ""; tokenCalls = 0; inputCalls = 0; resultCalls = 0; Trace.hook = new Observer();
            token = mode == 1 ? null : new TokenProbe(); input = mode == 2 ? null : new InputProbe();
            type = mode == 3 ? new Object() : token; value = mode == 4 ? new Object() : input;
            tokenValue = mode == 21 ? null : new Object(); inputValue = mode == 22 ? null : new Object();
            text = new String("owned array item");
            Object payload = mode == 14 ? new Object[] {new Object()} : mode == 15 ? new String[0] : mode == 16 ? null : new String[] {text};
            chosen = mode == 10 ? new Object() : mode == 13 ? null : new ResultProbe(payload);
            replacement = new ResultProbe(new String[] {text}); Trace.output = chosen;
            failure = mode == 6 || mode == 9 || mode == 20 ? new Checked() : mode == 7 || mode == 11 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Throwable caught = null; Object answer = null;
            try { answer = call(route); } catch (Throwable problem) { caught = problem; }
            String expected = mode == 3 || mode == 4 ? "" : "0,1,";
            boolean calledToken = !expected.isEmpty() && mode != 1 && mode != 5;
            if (calledToken && mode != 6) expected += "2,";
            boolean calledInput = calledToken && mode != 6 && mode != 7 && mode != 2;
            if (calledInput && mode != 8) expected += "3,";
            boolean returned = calledInput && mode != 8 && mode != 9 && mode != 10;
            if (returned) expected += "4,";
            if (returned && mode != 11) expected += "5,";
            check(events.equals(expected));
            check(tokenCalls == (calledToken ? 1 : 0) && inputCalls == (calledInput ? 1 : 0));
            check(resultCalls == (returned && mode != 11 && mode != 12 && mode != 13 && checked ? 1 : 0));
            if (mode == 5 || mode == 6 || mode == 7 || mode == 8 || mode == 9 || mode == 11 || mode == 12 || mode == 20 && checked) check(caught == failure);
            else if (mode == 1 || mode == 2 || checked && (mode == 13 || mode == 16)) check(caught instanceof NullPointerException);
            else if (mode == 3 || mode == 4 || mode == 10 || checked && mode == 14) check(caught instanceof ClassCastException);
            else if (checked && mode == 15) check(caught instanceof ArrayIndexOutOfBoundsException);
            else {
                check(caught == null && answer == (checked ? text : chosen));
                if (!checked && mode == 0) {
                    String changed = new String("owned changed item");
                    ((String[])((ResultProbe)answer).payload)[0] = changed;
                    check(((String[])((ResultProbe)chosen).payload)[0] == changed);
                }
            }
        }
        System.out.println("attempts=2,dependent-array-carrier=matched");
    }
}
