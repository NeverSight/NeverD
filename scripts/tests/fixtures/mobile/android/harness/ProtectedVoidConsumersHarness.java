import fixture.Base;
import fixture.Child;
import fixture.Core;
import fixture.Event;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

public final class ProtectedVoidConsumersHarness {
    public static final class Override extends Core<Object> {
        public int overrides;
        protected void accept(Event value) { overrides++; super.accept(value); }
    }
    private static int values, identities, states, failures;
    private static Method erased, typed;
    private static MethodHandle handle;
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value"); values++;
    }
    private static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("state"); states++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity"); identities++;
    }
    private static int overrides(Core<?> receiver) {
        return receiver instanceof Override ? ((Override) receiver).overrides : 0;
    }
    private static void invoke(int route, Core<?> receiver, Object event) throws Throwable {
        try {
            switch (route) {
                case 0: receiver.typed((Event) event); break;
                case 1: ((Base) receiver).dispatch(event); break;
                case 2: erased.invoke(receiver, event); break;
                case 3: handle.invoke(receiver, event); break;
                case 4: ProtectedVoidCaller.call(receiver, event); break;
                case 5: typed.invoke(receiver, event); break;
                default: throw new AssertionError("route");
            }
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        erased = Core.class.getDeclaredMethod("accept", Object.class);
        typed = Core.class.getDeclaredMethod("accept", Event.class);
        if (!erased.isBridge() || !erased.isSynthetic() || !Modifier.isPublic(erased.getModifiers())
                || Modifier.isFinal(Core.class.getModifiers())
                || Core.class.getTypeParameters().length != 1
                || Child.class.getTypeParameters().length != 1
                || (rebuilt ? !Modifier.isPublic(typed.getModifiers()) : !Modifier.isProtected(typed.getModifiers())))
            throw new AssertionError("original hierarchy or reported visibility");
        if (!Modifier.isProtected(Override.class.getDeclaredMethod("accept", Event.class).getModifiers()))
            throw new AssertionError("immutable external override changed");
        typed.setAccessible(true);
        handle = MethodHandles.publicLookup().unreflect(erased);
        Core<?>[] receivers = {new Core<Object>(), new Child<Object>(), new Override()};
        Event[] events = {null, new Event(), new Event()};
        for (Core<?> receiver : receivers)
            for (int route = 0; route < 6; route++)
                for (Event event : events)
                    for (int marker : new int[] {-9, 0, 17})
                        for (boolean throwing : new boolean[] {false, true}) {
                            int beforeCalls = receiver.calls, beforeVisits = receiver.visits;
                            int beforeOverrides = overrides(receiver), beforeValue = event == null ? 0 : event.value;
                            RuntimeException failure = throwing ? new IllegalStateException("owned failure") : null;
                            receiver.failure = failure; receiver.marker = marker;
                            try { invoke(route, receiver, event); if (throwing) throw new AssertionError("missing failure"); }
                            catch (RuntimeException caught) { if (!throwing) throw caught; identity(caught, failure); failures++; }
                            state(receiver.calls, beforeCalls + 1);
                            state(receiver.visits, beforeVisits + (route == 1 || route == 4 ? 1 : 0));
                            state(overrides(receiver), beforeOverrides + (receiver instanceof Override ? 1 : 0));
                            identity(receiver.last, event);
                            if (event != null) value(event.value, throwing ? beforeValue : marker);
                        }
        for (Core<?> receiver : receivers)
            for (int route : new int[] {1, 2, 3, 4})
                for (Object invalid : new Object[] {new Object(), "wrong type"}) {
                    int beforeCalls = receiver.calls, beforeVisits = receiver.visits, beforeOverrides = overrides(receiver);
                    Event beforeLast = receiver.last;
                    receiver.failure = new IllegalStateException("must not reach target");
                    try { invoke(route, receiver, invalid); throw new AssertionError("missing checked cast"); }
                    catch (ClassCastException expected) { failures++; }
                    state(receiver.calls, beforeCalls);
                    state(receiver.visits, beforeVisits + (route == 1 || route == 4 ? 1 : 0));
                    state(overrides(receiver), beforeOverrides);
                    identity(receiver.last, beforeLast);
                }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("public-erased-dispatch-and-protected-external-override=retained,typed-access-change=reported");
    }
}
