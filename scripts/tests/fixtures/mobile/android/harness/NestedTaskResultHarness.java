package fixture;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;

public final class NestedTaskResultHarness {
    private static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned nested task result mismatch");
    }
    public static final class Payload {}
    private static final class TrapCollection extends AbstractCollection<Callable<Payload>> {
        int touched, mutation;
        public Iterator<Callable<Payload>> iterator() { touched++; throw new AssertionError("unexpected owned traversal"); }
        public int size() { touched++; throw new AssertionError("unexpected owned size"); }
    }
    private static final class State implements InvocationHandler {
        final int operation, mode;
        final long timeout;
        final TimeUnit unit;
        final TrapCollection original = new TrapCollection();
        final Collection<Callable<Payload>> input;
        final Payload payload = new Payload();
        final Object output;
        final Throwable cause = new IllegalArgumentException("owned cause");
        final Throwable failure;
        Core core;
        int calls;
        State(int operation, int mode, int attempt) {
            this.operation = operation; this.mode = mode;
            this.timeout = attempt == 0 ? Long.MIN_VALUE + 13 : Long.MAX_VALUE - 7;
            this.unit = mode == 15 ? null : TimeUnit.NANOSECONDS;
            this.input = mode == 1 ? null : original;
            this.output = mode == 2 ? null : mode == 9 ? new Object() : payload;
            this.failure = mode == 4 ? new InterruptedException("owned interrupt")
                    : mode == 5 ? new ExecutionException(cause)
                    : mode == 6 ? new IllegalStateException("owned runtime")
                    : mode == 7 ? new AssertionError("owned error") : new TimeoutException("owned timeout");
        }
        boolean fails() { return (mode >= 4 && mode <= 7) || (mode == 12 && (operation & 1) != 0); }
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            require(method.getDeclaringClass() == (operation < 2 ? ExecutorService.class : Worker.class));
            require(method.getName().equals(operation < 2 ? "invokeAny" : "choose"));
            require(args.length == ((operation & 1) == 0 ? 1 : 3) && args[0] == input);
            if (args.length == 3) require(((Long) args[1]).longValue() == timeout && args[2] == unit);
            require(calls++ == 0 && original.touched == 0);
            require(Thread.currentThread().isInterrupted() == (mode == 14));
            if (fails()) throw failure;
            if (mode == 10) original.mutation = 73;
            if (mode == 11) { core.service = null; core.worker = null; }
            return output;
        }
    }
    private static Object call(int route, State state) throws Throwable {
        Core core = state.core; Collection<Callable<Payload>> input = state.input;
        if (route == 0) {
            if (state.operation == 0) return core.single(input);
            if (state.operation == 1) return core.scheduled(input, state.timeout, state.unit);
            if (state.operation == 2) return core.owned(input);
            return core.ownedScheduled(input, state.timeout, state.unit);
        }
        if (route == 1) return NestedTaskResultCaller.call(core, state.operation, input, state.timeout, state.unit);
        String name = new String[]{"single", "scheduled", "owned", "ownedScheduled"}[state.operation];
        Class<?>[] types = (state.operation & 1) == 0 ? new Class<?>[]{Collection.class}
                : new Class<?>[]{Collection.class, long.class, TimeUnit.class};
        Object[] args = (state.operation & 1) == 0 ? new Object[]{input} : new Object[]{input, state.timeout, state.unit};
        if (route == 2) {
            try { return Core.class.getMethod(name, types).invoke(core, args); }
            catch (InvocationTargetException ex) { throw ex.getCause(); }
        }
        List<Object> values = new ArrayList<Object>(); values.add(core); Collections.addAll(values, args);
        return MethodHandles.lookup().findVirtual(Core.class, name, MethodType.methodType(Object.class, types)).invokeWithArguments(values);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        require(Modifier.isVolatile(Core.class.getField("service").getModifiers()));
        require(Modifier.isVolatile(Core.class.getField("worker").getModifiers()));
        for (int attempt = 0; attempt < 2; ++attempt) {
            State state = new State(operation, mode, attempt);
            ExecutorService service = (ExecutorService) Proxy.newProxyInstance(Core.class.getClassLoader(), new Class<?>[]{ExecutorService.class}, state);
            Worker worker = (Worker) Proxy.newProxyInstance(Core.class.getClassLoader(), new Class<?>[]{Worker.class}, state);
            state.core = new Core(mode == 8 ? null : service, mode == 8 ? null : worker);
            Thread.interrupted(); if (mode == 14) Thread.currentThread().interrupt();
            Object result = null; Throwable caught = null;
            try { result = call(route, state); } catch (Throwable failure) { caught = failure; }
            boolean interrupted = Thread.currentThread().isInterrupted(); Thread.interrupted();
            require(interrupted == (mode == 14));
            if (state.fails()) {
                require(caught == state.failure && result == null);
                if (mode == 5) require(caught.getCause() == state.cause);
            } else if (mode == 8) require(caught != null && caught.getClass() == NullPointerException.class && result == null);
            else require(caught == null && result == state.output);
            require(state.calls == (mode == 8 ? 0 : 1) && state.original.touched == 0);
            if (mode == 10) require(state.original.mutation == 73);
            if (mode == 11) require(state.core.service == null && state.core.worker == null);
            if (result != null && mode != 13) {
                Payload payload = null; Throwable consumerFailure = null;
                try { payload = (Payload) result; } catch (Throwable failure) { consumerFailure = failure; }
                if (mode == 9) require(consumerFailure != null && consumerFailure.getClass() == ClassCastException.class);
                else require(consumerFailure == null && payload == state.payload);
            }
        }
        System.out.println("attempts=2,nested-task-result=matched");
    }
}
