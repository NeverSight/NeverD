import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public class UnaryVoidCarrierHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned unary void contract"); }
    static void call(int route, Provider provider, Factory<?> factory) throws Throwable {
        if (route == 0) Core.call(provider, factory);
        else if (route == 1) UnaryVoidCarrierCaller.call(provider, factory);
        else if (route == 2) {
            try { Core.class.getMethod("call", Provider.class, Factory.class).invoke(null, provider, factory); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else MethodHandles.publicLookup().findStatic(Core.class, "call",
                MethodType.methodType(void.class, Provider.class, Factory.class)).invoke(provider, factory);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.failure = new IllegalArgumentException("owned factory failure");
        Trace.fail = mode <= 5 ? mode : 0;
        int[] data = {7}, factories = {0};
        Object value = mode == 8 ? null : mode == 9 ? new Object() : data;
        Factory<Object> factory = mode == 7 ? null : () -> {
            factories[0]++; Trace.hit(2); return value;
        };
        Worker worker = variant == 0 ? new Worker() : new Child();
        Provider provider = mode == 6 ? null : worker;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0;
            int oldData = data[0], oldCalls = worker.calls, oldFactories = factories[0];
            Object oldLast = worker.last;
            Throwable thrown = null;
            try { call(route, provider, factory); } catch (Throwable failure) { thrown = failure; }
            boolean started = mode != 6;
            boolean body = started && !(variant == 1 && mode == 4);
            boolean made = body && mode != 1 && mode != 7;
            boolean stored = made && mode != 2;
            boolean returned = stored && mode != 3 && mode != 9;
            long expected = variant == 1 && started ? 4 : 0;
            if (body) expected = expected * 10 + 1;
            if (made) expected = expected * 10 + 2;
            if (stored && mode != 9) expected = expected * 10 + 3;
            if (variant == 1 && returned) expected = expected * 10 + 5;
            if (mode == 6 || mode == 7) require(thrown instanceof NullPointerException);
            else if (mode == 9) require(thrown instanceof ClassCastException);
            else if (mode >= 1 && mode <= 3 || variant == 1 && (mode == 4 || mode == 5)) require(thrown == Trace.failure);
            else require(thrown == null);
            require(Trace.events == expected && factories[0] == oldFactories + (made ? 1 : 0));
            require(worker.calls == oldCalls + (stored ? 1 : 0));
            require(worker.last == (stored ? value : oldLast));
            require(data[0] == oldData + (stored && mode != 8 && mode != 9 ? 1 : 0));
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
