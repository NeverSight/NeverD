import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.lang.reflect.Proxy;
public final class MangledFinalHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("mangled final dispatch changed"); }
    static Object call(Terminal receiver, Object value, int route, String name) throws Throwable {
        if (route == 0) return NamesKt.closed(receiver, value);
        if (route == 1) return MangledFinalCaller.call(receiver, value);
        if (route == 2) {
            try { return Terminal.class.getMethod(name, Object.class).invoke(receiver, value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findVirtual(Terminal.class, name,
                MethodType.methodType(Object.class, Object.class)).invoke(receiver, value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        String name = args[2];
        require(Modifier.isFinal(Terminal.class.getModifiers()));
        require(Modifier.isFinal(Terminal.class.getMethod(name, Object.class).getModifiers()));
        require(!Terminal.class.getMethod(name, Object.class).isSynthetic());
        Trace.failure = new RuntimeException("owned final failure");
        Trace.failAt = mode == 1 ? 4 : mode == 2 ? 3 : 0;
        int[] input = {7}, calls = {0}, distinct = {21};
        Object value = mode == 3 ? null : input;
        Object output = mode == 4 ? null : mode == 6 ? distinct : input;
        Api<Object> target = (Api<Object>) Proxy.newProxyInstance(Api.class.getClassLoader(), new Class<?>[]{Api.class},
                (proxy, method, values) -> {
                    require(method.getName().equals(name) && values.length == 1 && values[0] == value);
                    calls[0]++; Trace.hit(3); input[0]++; return output;
                });
        Terminal receiver = mode == 5 ? null : new Terminal(target);
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; int oldCalls = calls[0], oldValue = input[0];
            Throwable failure = null; Object result = null;
            try { result = call(receiver, value, route, name); } catch (Throwable error) { failure = error; }
            int expected = mode == 5 ? 0 : mode == 1 ? 4 : 43;
            boolean callback = mode != 5 && mode != 1;
            require(Trace.events == expected && calls[0] == oldCalls + (callback ? 1 : 0));
            require(input[0] == oldValue + (callback && mode != 2 ? 1 : 0));
            if (mode == 5) require(failure instanceof NullPointerException);
            else if (mode == 1 || mode == 2) require(failure == Trace.failure);
            else require(failure == null && result == output);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
