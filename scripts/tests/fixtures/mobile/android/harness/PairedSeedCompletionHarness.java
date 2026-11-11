package probe;
import fixture.*;
import fixture.api.Api;
import fixture.client.*;
import java.io.IOException;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
public final class PairedSeedCompletionHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("owned paired seed completion mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static void lower(Type actual, Type expected) {
        WildcardType wildcard = (WildcardType) actual;
        check(Arrays.equals(wildcard.getLowerBounds(), new Type[]{expected}));
        check(Arrays.equals(wildcard.getUpperBounds(), new Type[]{Object.class}));
    }
    static void metadata(Method method) {
        Class<?> declaring = method.getDeclaringClass();
        check(declaring.getName().equals("fixture.api.Base") && !Modifier.isPublic(declaring.getModifiers()));
        check(declaring.getSuperclass() == Object.class && declaring.getTypeParameters().length == 0);
        check(Api.class.getSuperclass().getSuperclass() == declaring && Modifier.isFinal(Api.class.getModifiers()));
        check(method.getModifiers() == (Modifier.PUBLIC | Modifier.STATIC | Modifier.FINAL));
        check(method.getExceptionTypes().length == 0 && method.getReturnType() == Object.class);
        TypeVariable<?>[] callbackVariables = Fn.class.getTypeParameters();
        check(callbackVariables.length == 4);
        for (Class<?> carrier : new Class<?>[]{Fn.class, Marker.class, Completion.class})
            for (TypeVariable<?> variable : carrier.getTypeParameters())
                check(Arrays.equals(variable.getBounds(), new Type[]{Object.class}));
        ParameterizedType marker = (ParameterizedType) Fn.class.getGenericInterfaces()[0];
        check(marker.getRawType() == Marker.class && marker.getActualTypeArguments()[0].equals(callbackVariables[3]));
        TypeVariable<Method>[] variables = method.getTypeParameters();
        check(variables.length == 3);
        for (TypeVariable<?> variable : variables) check(Arrays.equals(variable.getBounds(), new Type[]{Object.class}));
        Type[] parameters = method.getGenericParameterTypes();
        check(parameters.length == 4 && parameters[1].equals(variables[0]) && parameters[2].equals(variables[1]));
        ParameterizedType callback = (ParameterizedType) parameters[0];
        check(callback.getRawType() == Fn.class);
        Type[] arguments = callback.getActualTypeArguments();
        check(arguments.length == 4);
        lower(arguments[0], variables[0]); lower(arguments[1], variables[1]);
        ParameterizedType completion = (ParameterizedType) parameters[3];
        check(completion.getRawType() == Completion.class);
        lower(completion.getActualTypeArguments()[0], variables[2]);
        lower(arguments[2], completion);
        WildcardType output = (WildcardType) arguments[3];
        check(output.getLowerBounds().length == 0 && Arrays.equals(output.getUpperBounds(), new Type[]{Object.class}));
    }
    static Object invoke(int route, Method method, MethodHandle handle,
                         Fn<Object, Object, Completion<? super Object>, Object> callback,
                         Object left, Object right, Completion<Object> completion) throws Throwable {
        if (route == 0) return Core.call(callback, left, right, completion);
        if (route == 1) return PairedSeedCompletionCaller.call(callback, left, right, completion);
        Trace.hit(6);
        Object result;
        try {
            if (route == 2) result = method.invoke(null, callback, left, right, completion);
            else if (route == 3) result = (Object) handle.invokeExact((Fn) callback, left, right, (Completion) completion);
            else result = PairedSeedCompletionCaller.direct(callback, left, right, completion);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
        Trace.hit(0);
        return result;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Method method = Api.class.getMethod("invoke", Fn.class, Object.class, Object.class, Completion.class);
        metadata(method);
        method.setAccessible(true);
        MethodHandle handle = MethodHandles.publicLookup().findStatic(Api.class, "invoke",
            MethodType.methodType(Object.class, Fn.class, Object.class, Object.class, Completion.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object marker = new Object();
            long bits = (attempt == 0 ? 0x7ff8123456abcdefL : 0x8123456789abcdefL) ^ mode;
            Token token = new Token(marker, bits);
            check(token.payload == marker && token.bits == bits);
            Object left = mode == 5 ? null : token;
            Object right = mode == 6 ? null : mode == 7 ? left : new String("owned right");
            Object opaque = attempt == 0 ? token : new Object();
            Object context = new Object(), unset = new Object(), deferredValue = new Object();
            int[] called = {0}, completed = {0}, contexts = {0}, hooks = {0};
            Object[] accepted = {unset}, savedCompletion = {null}, nestedInputs = {new Object(), new Object(), null};
            Throwable failure = mode == 8 || mode == 9 ? new IOException("owned checked")
                : mode == 11 || mode == 12 ? new AssertionError("owned error") : new IllegalStateException("owned runtime");
            Completion<Object> completion = new Completion<Object>() {
                public Object context() { contexts[0]++; return context; }
                public void accept(Object value) {
                    completed[0]++; accepted[0] = value; Trace.hit(3);
                    if (mode == 12) raise(failure);
                }
            };
            Object expected = mode == 1 ? null : mode == 2 ? left : mode == 3 ? right : mode == 4 ? completion : opaque;
            Fn<Object, Object, Completion<? super Object>, Object> callback = (first, second, tail) -> {
                called[0]++; Trace.hit(2);
                check(first == left && second == right && tail == completion && tail.context() == context);
                check(Trace.first == left && Trace.second == right && Trace.completion == completion);
                if (mode >= 9 && mode <= 11) raise(failure);
                if (mode == 14) {
                    Object nestedResult = new Object(), nestedContext = new Object();
                    int[] nestedAccepted = {0};
                    Completion<Object> other = new Completion<Object>() {
                        public Object context() { return nestedContext; }
                        public void accept(Object value) { check(value == nestedResult); nestedAccepted[0]++; Trace.hit(3); }
                    };
                    nestedInputs[2] = other;
                    Fn<Object, Object, Completion<? super Object>, Object> inner = (a, b, c) -> {
                        Trace.hit(2);
                        check(a == nestedInputs[0] && b == nestedInputs[1] && c == other && c.context() == nestedContext);
                        c.accept(nestedResult); Trace.hit(4); return nestedResult;
                    };
                    check(Core.call(inner, nestedInputs[0], nestedInputs[1], other) == nestedResult && nestedAccepted[0] == 1);
                }
                if (mode == 15) savedCompletion[0] = tail;
                else tail.accept(expected);
                Trace.hit(4);
                return expected;
            };
            Trace.events = 0; Trace.first = Trace.second = Trace.result = unset; Trace.completion = null;
            Trace.beforeAction = Trace.afterAction = null;
            Runnable fail = () -> { hooks[0]++; raise(failure); };
            if (mode == 8) Trace.beforeAction = fail;
            if (mode == 13) Trace.afterAction = fail;
            Object result = unset;
            Throwable caught = null;
            try { result = invoke(route, method, handle, callback, left, right, completion); }
            catch (Throwable error) { caught = error; }
            boolean aborted = mode >= 8 && mode <= 13;
            check(aborted ? caught == failure && result == unset : caught == null && result == expected);
            check(Trace.first == (mode == 14 ? nestedInputs[0] : left));
            check(Trace.second == (mode == 14 ? nestedInputs[1] : right));
            check(Trace.completion == (mode == 14 ? nestedInputs[2] : completion));
            check(called[0] == (mode == 8 ? 0 : 1) && contexts[0] == called[0]);
            check(hooks[0] == (mode == 8 || mode == 13 ? 1 : 0));
            boolean reachedCompletion = mode < 8 || mode == 12 || mode == 13 || mode == 14;
            check(completed[0] == (reachedCompletion ? 1 : 0));
            check(accepted[0] == (reachedCompletion ? expected : unset));
            boolean reachedTail = mode < 8 || mode >= 13;
            check(Trace.result == (reachedTail ? expected : unset));
            long events = mode == 8 ? 61 : mode >= 9 && mode <= 11 ? 612 : mode == 12 ? 6123
                : mode == 13 ? 612345 : mode == 14 ? 6126123457034570L : mode == 15 ? 6124570 : 61234570;
            check(Trace.events == events);
            if (mode == 15) {
                check(savedCompletion[0] == completion);
                ((Completion<?>) savedCompletion[0]).accept(deferredValue);
                check(accepted[0] == deferredValue && completed[0] == 1 && Trace.events == events * 10 + 3);
            }
        }
        System.out.println("attempts=2,paired-seed-completion=matched");
    }
}
