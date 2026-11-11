import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

public final class ProtectedGenericResultsHarness {
    public static final class Effect implements Fn<Throwable, Throwable> {
        int calls;
        Throwable seen, output;
        RuntimeException failure;
        public Throwable apply(Throwable value) {
            calls++; seen = value;
            if (failure != null) throw failure;
            return output == null ? value : output;
        }
    }
    private static int values, identities, states, failures;
    private static Method erased, typed;
    private static MethodHandle erasedHandle, typedHandle;
    private static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("state: " + actual + ":" + expected); states++;
    }
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value"); values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity"); identities++;
    }
    @SuppressWarnings("unchecked")
    private static Fn<? super Throwable, ? extends Throwable> invoke(int route,
            Base<Fn<? super Throwable, ? extends Throwable>> receiver, Class<?> type) throws Throwable {
        try {
            switch (route) {
                case 0: return receiver.get(type);
                case 1: return (Fn<? super Throwable, ? extends Throwable>) erased.invoke(receiver, type);
                case 2: return (Fn<? super Throwable, ? extends Throwable>) erasedHandle.invoke(receiver, type);
                case 3: return (Fn<? super Throwable, ? extends Throwable>) typed.invoke(receiver, type);
                case 4: return (Fn<? super Throwable, ? extends Throwable>) typedHandle.invoke(receiver, type);
                case 5: return ProtectedGenericCaller.call(receiver, type);
                default: throw new AssertionError("route");
            }
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args[0].equals("rebuilt");
        Base<Fn<? super Throwable, ? extends Throwable>> receiver = Host.receiver;
        Class<?> owner = receiver.getClass();
        for (Method method : owner.getDeclaredMethods()) {
            if (!method.getName().equals("compute")) continue;
            if (method.getReturnType() == Object.class) erased = method;
            if (method.getReturnType() == Fn.class) typed = method;
        }
        if (erased == null || typed == null || !erased.isBridge() || !erased.isSynthetic()
                || !Modifier.isPublic(erased.getModifiers()) || !Modifier.isFinal(owner.getModifiers())
                || owner.isAnonymousClass() == rebuilt
                || (rebuilt ? !Modifier.isPublic(typed.getModifiers()) : !Modifier.isProtected(typed.getModifiers()))
                || !typed.getGenericParameterTypes()[0].getTypeName().equals("java.lang.Class<?>")
                || !typed.getGenericReturnType().getTypeName().equals("fixture.Fn<java.lang.Throwable, java.lang.Throwable>")
                || !owner.getGenericSuperclass().getTypeName().equals(
                    "fixture.Base<fixture.Fn<? super java.lang.Throwable, ? extends java.lang.Throwable>>"))
            throw new AssertionError("complete generic contract or reported source metadata");
        erased.setAccessible(true); typed.setAccessible(true);
        erasedHandle = MethodHandles.lookup().unreflect(erased);
        typedHandle = MethodHandles.lookup().unreflect(typed);
        Effect first = new Effect(), second = new Effect();
        @SuppressWarnings("unchecked") Fn<Throwable, Throwable>[] results = new Fn[]{null, first, second};
        RuntimeException problem = new IllegalStateException("owned-failure");
        for (int route = 0; route < 6; route++)
            for (Class<?> key : new Class<?>[]{null, Object.class, String.class, int[].class})
                for (Fn<Throwable, Throwable> result : results)
                    for (boolean throwing : new boolean[]{false, true}) {
                        State.value = result; State.failure = throwing ? problem : null;
                        Fn<Throwable, Throwable> last = State.last;
                        int before = State.calls, visits = receiver.visits;
                        boolean shouldFail = key == null || throwing;
                        try {
                            Fn<? super Throwable, ? extends Throwable> actual = invoke(route, receiver, key);
                            if (shouldFail) throw new AssertionError("failure missing");
                            identity(actual, result);
                            if (actual != null) {
                                Effect effect = (Effect) result;
                                Throwable input = new Throwable("owned-input"), output = new Throwable("owned-output");
                                effect.output = output; int calls = effect.calls;
                                identity(actual.apply(input), output); identity(effect.seen, input);
                                state(effect.calls, calls + 1);
                                output.addSuppressed(input); value(effect.output.getSuppressed().length, 1);
                                effect.failure = problem;
                                try { actual.apply(null); throw new AssertionError("callback failure missing"); }
                                catch (RuntimeException failure) { identity(failure, problem); failures++; }
                                identity(effect.seen, null); state(effect.calls, calls + 2);
                                effect.failure = null;
                            }
                        } catch (RuntimeException actual) {
                            if (!shouldFail) throw actual;
                            if (key == null) {
                                if (!(actual instanceof NullPointerException)) throw actual;
                            } else identity(actual, problem);
                            failures++;
                        }
                        state(State.calls, before + 1);
                        state(receiver.visits, visits + (route == 0 || route == 5 ? 1 : 0));
                        identity(State.type, key); identity(State.last, shouldFail ? last : result);
                    }
        for (int route = 0; route < 6; route++) {
            int before = State.calls;
            try { invoke(route, null, Object.class); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
            state(State.calls, before);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("closed-generic-signatures=retained,target-access-and-anonymous-source-binding=reported");
    }
}
