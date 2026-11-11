package fixture;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;

public final class NestedTaskTransferHarness {
    private static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned nested task transfer mismatch");
    }
    public static final class Payload {}
    private static final class TrapCollection extends AbstractCollection<Callable<Payload>> {
        int touched, mutation;
        public Iterator<Callable<Payload>> iterator() { touched++; throw new AssertionError("unexpected owned traversal"); }
        public int size() { touched++; throw new AssertionError("unexpected owned size"); }
    }
    @SuppressWarnings("rawtypes")
    private static final class OpaqueFuture implements Future {
        final Object result;
        int gets, other;
        OpaqueFuture(Object result) { this.result = result; }
        public Object get() { gets++; return result; }
        public Object get(long timeout, TimeUnit unit) { other++; throw new AssertionError("unexpected owned timed get"); }
        public boolean cancel(boolean value) { other++; throw new AssertionError("unexpected owned cancel"); }
        public boolean isCancelled() { other++; throw new AssertionError("unexpected owned cancellation"); }
        public boolean isDone() { other++; throw new AssertionError("unexpected owned completion"); }
    }
    private static final class State implements InvocationHandler {
        final int operation, mode;
        final long timeout;
        final TimeUnit unit;
        final TrapCollection original = new TrapCollection();
        final Collection<Callable<Payload>> input;
        final List<Object> output = new ArrayList<Object>();
        final Payload payload = new Payload();
        final OpaqueFuture future;
        final Throwable failure;
        Core core;
        int calls;
        State(int operation, int mode, int attempt) {
            this.operation = operation; this.mode = mode;
            this.timeout = attempt == 0 ? Long.MIN_VALUE + 13 : Long.MAX_VALUE - 7;
            this.unit = mode == 15 ? null : TimeUnit.NANOSECONDS;
            this.input = mode == 1 ? null : original;
            this.future = new OpaqueFuture(mode == 9 ? new Object() : payload);
            if (mode != 3) output.add(mode == 8 ? new Object() : future);
            this.failure = mode == 4 ? new InterruptedException("owned interrupt")
                    : mode == 5 ? new IllegalStateException("owned runtime") : new AssertionError("owned error");
        }
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            require(method.getDeclaringClass() == (operation < 2 ? ExecutorService.class : Worker.class));
            require(method.getName().equals(operation < 2 ? "invokeAll" : "batch"));
            require(args.length == ((operation & 1) == 0 ? 1 : 3) && args[0] == input);
            if (args.length == 3) require(((Long) args[1]).longValue() == timeout && args[2] == unit);
            require(calls++ == 0 && original.touched == 0 && future.gets == 0 && future.other == 0);
            require(Thread.currentThread().isInterrupted() == (mode == 14));
            if (mode >= 4 && mode <= 6) throw failure;
            if (mode == 11) original.mutation = 73;
            if (mode == 12) { core.service = null; core.worker = null; }
            return mode == 2 ? null : output;
        }
    }
    @SuppressWarnings("unchecked")
    private static List<Future<Payload>> call(int route, State state) throws Throwable {
        Core core = state.core; Collection<Callable<Payload>> input = state.input;
        if (route == 0) {
            if (state.operation == 0) return core.batch(input);
            if (state.operation == 1) return core.scheduled(input, state.timeout, state.unit);
            if (state.operation == 2) return core.owned(input);
            return core.ownedScheduled(input, state.timeout, state.unit);
        }
        if (route == 1) return NestedTaskTransferCaller.call(core, state.operation, input, state.timeout, state.unit);
        String name = new String[]{"batch", "scheduled", "owned", "ownedScheduled"}[state.operation];
        Class<?>[] types = (state.operation & 1) == 0 ? new Class<?>[]{Collection.class}
                : new Class<?>[]{Collection.class, long.class, TimeUnit.class};
        Object[] args = (state.operation & 1) == 0 ? new Object[]{input} : new Object[]{input, state.timeout, state.unit};
        if (route == 2) {
            try { return (List<Future<Payload>>) Core.class.getMethod(name, types).invoke(core, args); }
            catch (InvocationTargetException ex) { throw ex.getCause(); }
        }
        List<Object> values = new ArrayList<Object>(); values.add(core); Collections.addAll(values, args);
        return (List<Future<Payload>>) MethodHandles.lookup().findVirtual(Core.class, name, MethodType.methodType(List.class, types)).invokeWithArguments(values);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        require(Modifier.isVolatile(Core.class.getField("service").getModifiers()));
        require(Modifier.isVolatile(Core.class.getField("worker").getModifiers()));
        for (int attempt = 0; attempt < 2; ++attempt) {
            State state = new State(operation, mode, attempt);
            ExecutorService service = (ExecutorService) Proxy.newProxyInstance(Core.class.getClassLoader(), new Class<?>[]{ExecutorService.class}, state);
            Worker worker = (Worker) Proxy.newProxyInstance(Core.class.getClassLoader(), new Class<?>[]{Worker.class}, state);
            state.core = new Core(mode == 7 ? null : service, mode == 7 ? null : worker);
            Thread.interrupted(); if (mode == 14) Thread.currentThread().interrupt();
            List<Future<Payload>> result = null; Throwable caught = null;
            try { result = call(route, state); } catch (Throwable failure) { caught = failure; }
            boolean interrupted = Thread.currentThread().isInterrupted(); Thread.interrupted();
            require(interrupted == (mode == 14));
            if (mode >= 4 && mode <= 6) require(caught == state.failure && result == null);
            else if (mode == 7) require(caught != null && caught.getClass() == NullPointerException.class && result == null);
            else require(caught == null && result == (Object)(mode == 2 ? null : state.output));
            require(state.calls == (mode == 7 ? 0 : 1));
            require(state.original.touched == 0 && state.future.gets == 0 && state.future.other == 0);
            if (mode == 11) require(state.original.mutation == 73);
            if (mode == 12) require(state.core.service == null && state.core.worker == null);
            if (result != null && mode != 3 && mode != 13) {
                Payload payload = null; Throwable consumerFailure = null;
                try { payload = result.get(0).get(); } catch (Throwable failure) { consumerFailure = failure; }
                if (mode == 8 || mode == 9) require(consumerFailure != null && consumerFailure.getClass() == ClassCastException.class);
                else require(consumerFailure == null && payload == state.payload);
                require(state.future.gets == (mode == 8 ? 0 : 1) && state.future.other == 0);
            }
        }
        System.out.println("attempts=2,nested-task-transfer=matched");
    }
}
