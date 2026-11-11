import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class ClosedGetterHarness {
    public static class GetterChild extends Middle {
        public Box<Throwable> getItems() {
            State.calls += 10;
            State.receiver = this;
            if (State.failure != null) throw State.failure;
            return data;
        }
    }
    public static class BridgeChild extends Middle {
        public Box items() {
            State.calls += 100;
            State.receiver = this;
            if (State.failure != null) throw State.failure;
            return data;
        }
    }
    private static int values, identities, states, failures;
    private static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    private static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    private static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0;
        Core core = new Core();
        Middle[] receivers = {new Middle(), core, new GetterChild(), new BridgeChild(), null};
        Object mark = new Object();
        Throwable a = new Throwable("a"), b = new Throwable("b");
        Box<Throwable> first = new Box<Throwable>(a), second = new Box<Throwable>(b);
        RuntimeException markerFailure = new IllegalArgumentException("read");
        Method bridge = Middle.class.getMethod("items"), target = Middle.class.getMethod("getItems");
        value(Core.class.getTypeParameters().length == 0);
        value(Base.class.getMethod("echo", Object.class).getTypeParameters().length == 1);
        value(target.getTypeParameters().length == 0);
        value(target.getGenericReturnType().getTypeName().equals("fixture.Box<java.lang.Throwable>"));
        value(bridge.getGenericReturnType().getTypeName().equals("fixture.Box"));
        value(bridge.isBridge() == !rebuilt);
        value(bridge.isSynthetic() == !rebuilt);
        MethodHandle bh = MethodHandles.lookup().findVirtual(Middle.class, "items", MethodType.methodType(Box.class));
        MethodHandle th = MethodHandles.lookup().findVirtual(Middle.class, "getItems", MethodType.methodType(Box.class));
        for (Middle receiver : receivers) if (receiver != null)
            for (Object item : new Object[]{null, mark, first}) same(receiver.echo(item), item);
        for (int route = 0; route < 7; route++)
            for (Middle receiver : receivers)
                for (Box<Throwable> data : new Box[]{null, first, second})
                    for (boolean fail : new boolean[]{false, true}) {
                        if (receiver != null) receiver.data = data;
                        State.calls = 0;
                        State.receiver = mark;
                        State.failure = fail ? markerFailure : null;
                        Throwable failure = null;
                        Box<Throwable> result = null;
                        try {
                            if (route == 0) result = receiver.items();
                            else if (route == 1) result = ((Base)receiver).items();
                            else if (route == 2) result = ClosedGetterCaller.read(receiver);
                            else if (route == 3) result = (Box<Throwable>)bridge.invoke(receiver);
                            else if (route == 4) result = (Box)bh.invokeExact(receiver);
                            else if (route == 5) result = (Box<Throwable>)target.invoke(receiver);
                            else result = (Box)th.invokeExact(receiver);
                        } catch (InvocationTargetException error) { failure = error.getCause(); }
                        catch (Throwable error) { failure = error; }
                        int calls = receiver == null ? 0 : receiver instanceof GetterChild ? 10
                                : receiver instanceof BridgeChild && route < 5 ? 100 : 1;
                        state(State.calls == calls);
                        same(State.receiver, receiver == null ? mark : receiver);
                        if (receiver == null) { state(failure instanceof NullPointerException); failures++; }
                        else {
                            same(receiver.data, data);
                            if (fail) { same(failure, markerFailure); failures++; }
                            else {
                                state(failure == null);
                                same(result, data);
                                if (result != null) {
                                    Throwable saved = result.value;
                                    result.value = markerFailure;
                                    same(data.value, markerFailure);
                                    result.value = saved;
                                }
                            }
                        }
                        same(first.value, a);
                        same(second.value, b);
                    }
        State.calls = 0;
        State.failure = null;
        core.data = first;
        same(((Api)core).items(), first);
        same(State.receiver, core);
        state(State.calls == 1);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("inherited-and-external-override-dispatch=matched,nullable-mutable-results=retained,original-signatures=retained,bridge-flags=reported");
    }
}
