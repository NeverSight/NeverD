import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class StatelessMethodContractsHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned anonymous method contract"); }
    static Object call(int route, boolean fixed, Api self, Class token, Object value) throws Throwable {
        if (route == 0) return fixed ? self.inspect((Carrier) value) : self.echo(token, value);
        if (route == 1) return fixed ? StatelessMethodContractsCaller.inspect(self, (Carrier) value)
                                    : StatelessMethodContractsCaller.echo(self, token, value);
        String name = fixed ? "inspect" : "echo";
        Class<?>[] parameters = fixed ? new Class<?>[]{Carrier.class} : new Class<?>[]{Class.class, Object.class};
        if (route == 3) {
            MethodHandle handle = MethodHandles.publicLookup().findVirtual(Api.class, name,
                MethodType.methodType(fixed ? Carrier.class : Object.class, parameters));
            return fixed ? handle.invoke(self, value) : handle.invoke(self, token, value);
        }
        Method method = (route == 4 ? Owner.shared.getClass() : Api.class).getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        try { return fixed ? method.invoke(self, value) : method.invoke(self, token, value); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[2]);
        boolean fixed = Boolean.parseBoolean(args[1]);
        Api shared = Owner.shared;
        Method echo = shared.getClass().getDeclaredMethod("echo", Class.class, Object.class);
        TypeVariable<?>[] formals = echo.getTypeParameters();
        require(formals.length == 2 && formals[0].getBounds()[0] == Object.class
                && formals[1].getBounds()[0] == Object.class && echo.getGenericReturnType().equals(formals[0]));
        require(echo.getGenericParameterTypes()[1].equals(formals[0]));
        require(shared.getClass().getDeclaredFields().length == 0);
        Object sentinel = new Object();
        RuntimeException problem = new IllegalStateException("owned callback failure");
        for (int attempt = 0; attempt < 2; attempt++) {
            Carrier<Number, String> carrier = new Carrier<>(); carrier.first = attempt; carrier.second = "before";
            Object value = mode == 1 ? null : fixed ? carrier : new Object();
            Class token = mode == 1 ? null : Object.class;
            Api self = mode == 7 ? null : shared;
            int[] callbacks = {0};
            Trace.events = 0; Trace.receiver = sentinel; Trace.input = sentinel; Trace.token = sentinel;
            Trace.failure = problem; Trace.failAt = mode == 3 ? (fixed ? 4 : 1) : mode == 4 ? 2 : mode == 6 ? 3 : 0;
            Trace.observer = mode == 2 ? null : (seen, type) -> {
                require(Trace.receiver == shared && Trace.input == value && seen == value
                        && Trace.token == (fixed ? null : token) && type == Trace.token);
                callbacks[0]++; if (mode == 5) throw problem;
            };
            int before = Trace.calls; Object result = sentinel; Throwable failure = null;
            try { result = call(route, fixed, self, token, value); }
            catch (Throwable error) { failure = error; }
            boolean written = mode != 3 && mode != 7;
            int first = fixed ? 4 : 1;
            int events = mode == 7 ? 0 : mode == 3 ? first
                : mode == 2 || mode == 4 || mode == 5 ? first * 10 + 2 : first * 100 + 23;
            require(Trace.events == events && Trace.calls == before + (written ? 1 : 0));
            require(Trace.receiver == (written ? shared : sentinel)
                    && Trace.input == (written ? value : sentinel)
                    && Trace.token == (written ? (fixed ? null : token) : sentinel));
            require(callbacks[0] == (mode == 0 || mode == 1 || mode == 5 || mode == 6 ? 1 : 0));
            if (mode == 0 || mode == 1) require(failure == null && result == value);
            else if (mode == 2 || mode == 7) require(failure instanceof NullPointerException);
            else require(failure == problem);
            if (written && fixed && value != null) { carrier.second = "after"; require(((Carrier) Trace.input).second == carrier.second); }
            require(Owner.shared == shared);
            System.out.println("route=" + route + ",fixed=" + fixed + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + events);
        }
    }
}
