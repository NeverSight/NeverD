import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class SpecializedDependentBoundsHarness {
    static int identities, states, failures;
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void check(boolean value) { if (!value) throw new AssertionError("state or scope"); states++; }
    static int calls(Object self) { return self instanceof Core ? ((Core)self).calls : ((Host.Worker)self).calls; }
    static Object seen(Object self) { return self instanceof Core ? ((Core)self).seen : ((Host.Worker)self).seen; }
    static void setFailure(Object self, RuntimeException failure) {
        if (self instanceof Core) ((Core)self).failure = failure; else ((Host.Worker)self).failure = failure;
    }
    static Object reflected(Method method, Object self, Object[] input, Object tag, Sink sink) throws Throwable {
        try { return method.invoke(self, input, tag, sink); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static Object call(int route, boolean nested, Object self, Object[] input, Object tag, Sink sink) throws Throwable {
        Class<?> owner = nested ? Host.Worker.class : Core.class;
        switch (route) {
        case 0: return nested ? ((Host.Worker)self).copy((Value[])input, (Value)tag, sink) : ((Core)self).copy((Value[])input, (Value)tag, sink);
        case 1: return ((Api<Value>)self).copy((Value[])input, (Value)tag, sink);
        case 2: return ((Api)self).copy(input, tag, sink);
        case 3: return reflected(owner.getMethod("copy", Value[].class, Value.class, Sink.class), self, input, tag, sink);
        case 4: return reflected(owner.getMethod("copy", Object[].class, Object.class, Sink.class), self, input, tag, sink);
        case 5: return MethodHandles.publicLookup().findVirtual(owner, "copy", MethodType.methodType(Value[].class, Value[].class, Value.class, Sink.class)).invoke(self, input, tag, sink);
        case 6: return MethodHandles.publicLookup().findVirtual(owner, "copy", MethodType.methodType(Object[].class, Object[].class, Object.class, Sink.class)).invoke(self, input, tag, sink);
        case 7: return SpecializedDependentBoundsCaller.erased((Api)self, input, tag, sink);
        default: return SpecializedDependentBoundsCaller.typed(self, nested, (Value[])input, (Value)tag, sink);
        }
    }
    interface Action { void run() throws Throwable; }
    static void failure(Action action, Class<?> kind, Throwable original) throws Throwable {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (Throwable problem) {
            if (problem.getClass() != kind) throw new AssertionError("failure order", problem);
            if (original != null) same(problem, original);
            failures++;
        }
    }
    static void signature(Class<?> owner, Class<?> erased, Type root) throws Exception {
        Method method = owner.getMethod("copy", java.lang.reflect.Array.newInstance(erased, 0).getClass(), erased, Sink.class);
        TypeVariable<?>[] variables = method.getTypeParameters();
        check(variables.length == 2 && variables[0].getBounds()[0].equals(root)
            && variables[1].getBounds()[0].equals(variables[0])
            && variables[0].getGenericDeclaration().equals(method)
            && variables[1].getGenericDeclaration().equals(method));
        check(((GenericArrayType)method.getGenericParameterTypes()[0]).getGenericComponentType().equals(variables[0]));
        check(method.getGenericParameterTypes()[1].equals(variables[1]));
        check(((GenericArrayType)method.getGenericReturnType()).getGenericComponentType().equals(variables[0]));
        WildcardType callback = (WildcardType)((ParameterizedType)method.getGenericParameterTypes()[2]).getActualTypeArguments()[0];
        check(callback.getLowerBounds().length == 1 && callback.getLowerBounds()[0].equals(variables[0]));
        check(owner.getMethod("resume", erased, Sink.class).getTypeParameters()[0].getBounds()[0].equals(root));
    }
    public static void main(String[] args) throws Throwable {
        signature(Api.class, Object.class, Api.class.getTypeParameters()[0]);
        signature(Core.class, Value.class, Value.class);
        signature(Host.Worker.class, Value.class, Value.class);
        for (boolean nested : new boolean[]{false, true}) {
            Host host = new Host();
            Object self = nested ? host.new Worker() : new Core();
            Value first = new Value(), second = new Value();
            for (int route = 0; route < 9; route++) {
                final int selected = route;
                for (Value[] input : new Value[][]{null, new Value[0], new Value[]{first, second}})
                    for (Value tag : new Value[]{null, first, second}) {
                        int before = calls(self), outer = host.ticks;
                        int[] callbackCalls = {0};
                        Sink<Value> sink = (value, array) -> {
                            check(calls(self) == before + 1); same(seen(self), tag);
                            same(value, tag); same(array, input); callbackCalls[0]++;
                            if (input != null && input.length != 0) input[0] = tag;
                            if (value != null) value.state++;
                        };
                        int state = tag == null ? 0 : tag.state;
                        same(call(route, nested, self, input, tag, sink), input);
                        same(seen(self), tag); check(calls(self) == before + 1 && callbackCalls[0] == 1);
                        check(host.ticks == outer + (nested ? 1 : 0));
                        if (tag != null) check(tag.state == state + 1);
                        if (input != null && input.length != 0) same(input[0], tag);
                    }
                int before = calls(self), outer = host.ticks;
                Value[] input = {first};
                int[] callbacks = {0};
                Sink<Value> sink = (value, array) -> callbacks[0]++;
                RuntimeException original = new IllegalStateException("owned failure");
                setFailure(self, original);
                failure(() -> call(selected, nested, self, input, first, sink), original.getClass(), original);
                check(calls(self) == before + 1 && callbacks[0] == 0); same(seen(self), first);
                setFailure(self, null);
                Sink<Value> throwing = (value, array) -> { callbacks[0]++; throw original; };
                failure(() -> call(selected, nested, self, input, first, throwing), original.getClass(), original);
                check(calls(self) == before + 2 && callbacks[0] == 1);
                failure(() -> call(selected, nested, self, input, first, null), NullPointerException.class, null);
                check(calls(self) == before + 3 && callbacks[0] == 1);
                failure(() -> call(selected, nested, null, input, first, sink), NullPointerException.class, null);
                check(calls(self) == before + 3 && host.ticks == outer + (nested ? 3 : 0));
            }
            for (int route : new int[]{2, 4, 6, 7}) {
                final int selected = route; int before = calls(self), outer = host.ticks;
                failure(() -> call(selected, nested, self, new Object[0], new Object(), null), ClassCastException.class, null);
                failure(() -> call(selected, nested, self, new Value[0], new Object(), null), ClassCastException.class, null);
                failure(() -> call(selected, nested, null, new Object[0], new Object(), null), NullPointerException.class, null);
                check(calls(self) == before && host.ticks == outer);
            }
            for (Value tag : new Value[]{null, first, second}) {
                int before = calls(self), outer = host.ticks; int[] callbacks = {0};
                Sink<Value> sink = (value, array) -> { same(value, tag); same(array, null); callbacks[0]++; };
                SpecializedDependentBoundsCaller.resume((Api)self, tag, sink);
                check(calls(self) == before + 1 && callbacks[0] == 1 && host.ticks == outer + (nested ? 1 : 0));
                same(((Api)self).value(tag), tag);
            }
            int before = calls(self);
            failure(() -> SpecializedDependentBoundsCaller.resume((Api)self, new Object(), null), ClassCastException.class, null);
            check(calls(self) == before);
        }
        System.out.println("identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("specialized-method-bounds-original-bridges-captured-owner-and-callback-effects=retained");
    }
}
