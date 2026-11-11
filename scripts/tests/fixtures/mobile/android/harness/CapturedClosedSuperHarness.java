import fixture.Scope;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.concurrent.atomic.AtomicReference;
import kotlin.ResultKt;
import kotlin.coroutines.*;
import kotlin.coroutines.intrinsics.IntrinsicsKt;
import kotlin.coroutines.jvm.internal.BaseContinuationImpl;
import kotlin.jvm.internal.FunctionBase;
import kotlin.jvm.functions.*;

public final class CapturedClosedSuperHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned captured closed super");
    }
    static final class Receiver implements Scope {}
    static final class State implements Continuation<Object> {
        int events, seeds, operations, completions;
        Object value, chosen, initial = new Object(), middle = new Object(), after = new Object();
        Object completed;
        Throwable completedFailure;
        final RuntimeException failure = new IllegalArgumentException("owned failure");
        final Error error = new LinkageError("owned error");
        final Scope receiver = new Receiver();
        final AtomicReference<Object> holder = new AtomicReference<Object>(initial);
        Continuation<?> suspended;
        final int mode;
        State(int mode) {
            this.mode = mode;
            value = mode == 1 ? null : mode == 2 ? "owned value" : mode == 3 ? new Object[1] : new Object();
            chosen = mode == 4 ? null : mode == 5 ? value : new Object();
        }
        public CoroutineContext getContext() { return EmptyCoroutineContext.INSTANCE; }
        public void resumeWith(Object result) {
            check(holder.get() == after);
            completions++; events = events * 10 + 3;
            try { ResultKt.throwOnFailure(result); completed = result; }
            catch (Throwable caught) { completedFailure = caught; }
        }
    }
    static final class Seed implements Function1<Scope,Object> {
        final State state;
        Seed(State state) { this.state = state; }
        public Object invoke(Scope receiver) {
            check(receiver == state.receiver);
            state.seeds++; state.events = state.events * 10 + 1;
            state.holder.set(state.middle);
            if (state.mode == 6) throw state.failure;
            if (state.mode == 7) throw state.error;
            return state.value;
        }
    }
    static final class Operation implements Function2<Object,Continuation<? super Object>,Object> {
        final State state;
        Operation(State state) { this.state = state; }
        public Object invoke(Object value, Continuation<? super Object> continuation) {
            check(value == state.value && state.holder.get() == state.value);
            check(continuation instanceof BaseContinuationImpl);
            check(((BaseContinuationImpl)continuation).getCompletion() == state);
            state.operations++; state.events = state.events * 10 + 2;
            state.holder.set(state.after); state.suspended = continuation;
            if (state.mode == 8) throw state.failure;
            if (state.mode == 9) throw state.error;
            return state.mode >= 10 && state.mode <= 12 ? IntrinsicsKt.getCOROUTINE_SUSPENDED() : state.chosen;
        }
    }
    static Object reflect(Method method, Object receiver, Object... args) throws Throwable {
        method.setAccessible(true);
        try { return method.invoke(receiver, args); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    static Object make(String name, int route, State state, Seed seed, Operation operation) throws Throwable {
        Class<?> owner = Class.forName("fixture.Factory");
        Method method = owner.getDeclaredMethod(name, AtomicReference.class, Function1.class, Function2.class);
        Object actualSeed = state.mode == 13 ? null : seed;
        Object actualOperation = state.mode == 14 ? null : operation;
        if ((route & 1) == 0) return reflect(method, null, state.holder, actualSeed, actualOperation);
        MethodHandle handle = MethodHandles.publicLookup().unreflect(method);
        return handle.invokeWithArguments(state.holder, actualSeed, actualOperation);
    }
    @SuppressWarnings("unchecked")
    static Object invoke(int route, Object function, Scope scope, Continuation<Object> completion) throws Throwable {
        if (route == 0) return ((Function2<Scope,Continuation<Object>,Object>)function).invoke(scope, completion);
        if (route == 1) return reflect(Function2.class.getMethod("invoke", Object.class, Object.class), function, scope, completion);
        if (route == 2) {
            MethodHandle handle = MethodHandles.publicLookup().findVirtual(Function2.class, "invoke",
                MethodType.methodType(Object.class, Object.class, Object.class));
            return (Object)handle.invokeExact((Function2)function, (Object)scope, (Object)completion);
        }
        return reflect(function.getClass().getDeclaredMethod("invoke", Scope.class, Continuation.class), function, scope, completion);
    }
    static void captures(Object function, State state, Seed seed, Operation operation) throws Exception {
        int found = 0;
        for (Field field : function.getClass().getDeclaredFields()) {
            if (field.getType() != Function1.class && field.getType() != Function2.class && field.getType() != AtomicReference.class) continue;
            check(Modifier.isFinal(field.getModifiers()) && !Modifier.isStatic(field.getModifiers()));
            field.setAccessible(true);
            Object wanted = field.getType() == Function1.class ? seed : field.getType() == Function2.class ? operation : state.holder;
            check(field.get(function) == wanted); found++;
        }
        check(found == 3 && ((FunctionBase<?>)function).getArity() == 2);
        check(((BaseContinuationImpl)function).getCompletion() == null);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            State state = new State(mode);
            Seed seed = new Seed(state); Operation operation = new Operation(state);
            Object function = null, actual = null; Throwable caught = null;
            try {
                function = make(args[2], route, state, seed, operation);
                captures(function, state, seed, operation);
                check(state.holder.get() == state.initial && state.events == 0);
                actual = invoke(route, function, state.receiver, state);
            } catch (Throwable failure) { caught = failure; }
            if (mode == 13 || mode == 14) {
                check(caught instanceof NullPointerException && function == null && actual == null);
                check(state.seeds == 0 && state.operations == 0 && state.holder.get() == state.initial);
            } else if (mode == 6 || mode == 7) {
                check(caught == (mode == 6 ? state.failure : state.error) && actual == null);
                check(state.seeds == 1 && state.operations == 0 && state.holder.get() == state.middle);
            } else {
                check(state.seeds == 1 && state.operations == 1 && state.holder.get() == state.after);
                if (mode == 8 || mode == 9) check(caught == (mode == 8 ? state.failure : state.error) && actual == null);
                else if (mode >= 10 && mode <= 12) {
                    check(caught == null && actual == IntrinsicsKt.getCOROUTINE_SUSPENDED());
                    check(state.completions == 0 && state.suspended != function);
                    Object result = mode == 10 ? state.chosen : ResultKt.createFailure(mode == 11 ? state.failure : state.error);
                    state.suspended.resumeWith(result);
                    check(state.completions == 1 && state.events == 123 && state.seeds == 1 && state.operations == 1);
                    check(state.completed == (mode == 10 ? state.chosen : null));
                    check(state.completedFailure == (mode == 10 ? null : mode == 11 ? state.failure : state.error));
                } else check(caught == null && actual == state.chosen);
            }
            check(state.completions == (mode >= 10 && mode <= 12 ? 1 : 0));
            int events = mode >= 13 ? 0 : mode == 6 || mode == 7 ? 1 : mode >= 10 && mode <= 12 ? 123 : 12;
            check(state.events == events);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
