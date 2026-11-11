import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class InheritedContractImplementationsHarness {
    static int identities, states, failures;
    static final Method echoBase, echoApi, inspectBase, inspectApi;
    static final MethodHandle echoBaseHandle, echoApiHandle, inspectBaseHandle, inspectApiHandle;
    static {
        try {
            echoBase = Base.class.getMethod("echo", Class.class, Object.class);
            echoApi = Api.class.getMethod("echo", Class.class, Object.class);
            inspectBase = Base.class.getMethod("inspect", Carrier.class);
            inspectApi = Api.class.getMethod("inspect", Carrier.class);
            MethodHandles.Lookup lookup = MethodHandles.publicLookup();
            echoBaseHandle = lookup.unreflect(echoBase); echoApiHandle = lookup.unreflect(echoApi);
            inspectBaseHandle = lookup.unreflect(inspectBase); inspectApiHandle = lookup.unreflect(inspectApi);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static class Leaf extends Core {
        public <X, Y> X echo(Class<X> token, X input) { calls += 2; return super.echo(token, input); }
        public Carrier<Number, String> inspect(Carrier<Number, String> input) { calls += 2; return super.inspect(input); }
    }
    static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("inherited implementation changed identity");
        identities++;
    }
    static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("inherited implementation changed effects");
        states++;
    }
    static Object reflected(Method method, Core self, Object... input) throws Throwable {
        try { return method.invoke(self, input); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static Object echo(int route, Core self, Class token, Object input) throws Throwable {
        switch (route) {
        case 0: return self.echo(token, input);
        case 1: return ((Api) self).echo(token, input);
        case 2: return ((Base) self).echo(token, input);
        case 3: return reflected(echoBase, self, token, input);
        case 4: return reflected(echoApi, self, token, input);
        case 5: return echoBaseHandle.invoke(self, token, input);
        case 6: return echoApiHandle.invoke(self, token, input);
        default: return originalCaller(self, token, input);
        }
    }
    static <T, U> T originalCaller(Api self, Class<T> token, T input) { return self.echo(token, input); }
    static Object inspect(int route, Core self, Carrier input) throws Throwable {
        switch (route) {
        case 0: return self.inspect(input);
        case 1: return ((Api) self).inspect(input);
        case 2: return ((Base) self).inspect(input);
        case 3: return reflected(inspectBase, self, input);
        case 4: return reflected(inspectApi, self, input);
        case 5: return inspectBaseHandle.invoke(self, input);
        case 6: return inspectApiHandle.invoke(self, input);
        default: return originalInspect(self, input);
        }
    }
    static Carrier<Number, String> originalInspect(Api self, Carrier<Number, String> input) { return self.inspect(input); }
    interface Run { void run() throws Throwable; }
    static void failure(Run action, Class<?> type, Throwable original) throws Throwable {
        try { action.run(); throw new AssertionError("missing inherited failure"); }
        catch (Throwable error) {
            if (error.getClass() != type) throw new AssertionError("inherited failure order changed", error);
            if (original != null) same(error, original);
            failures++;
        }
    }
    static void metadata(boolean original, boolean synthetic) throws Exception {
        for (Method method : new Method[]{echoBase, inspectBase})
            if (method.isBridge() != original || method.isSynthetic() != (original && synthetic))
                throw new AssertionError("undisclosed original implementation flag change");
        if (Core.class.getMethod("echo", Class.class, Object.class).getDeclaringClass() != Base.class
                || Core.class.getMethod("inspect", Carrier.class).getDeclaringClass() != Base.class
                || java.util.Arrays.stream(Core.class.getDeclaredMethods()).filter(m -> !Modifier.isPrivate(m.getModifiers())).count() != 1
                || java.util.Arrays.stream(Root.class.getDeclaredMethods()).filter(m -> !Modifier.isPrivate(m.getModifiers())).count() != 1)
            throw new AssertionError("inherited provider was copied or moved");
        for (Method method : new Method[]{echoBase, echoApi}) {
            TypeVariable<?>[] variables = method.getTypeParameters();
            if (variables.length != 2 || variables[0].getBounds()[0] != Object.class
                    || variables[1].getBounds()[0] != Object.class
                    || !variables[0].getGenericDeclaration().equals(method)
                    || !method.getGenericParameterTypes()[1].equals(variables[0])
                    || !method.getGenericReturnType().equals(variables[0])
                    || !((ParameterizedType) method.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(variables[0]))
                throw new AssertionError("complete inherited method-formal bound changed");
        }
        if (!inspectBase.getGenericReturnType().getTypeName().equals("fixture.Carrier<java.lang.Number, java.lang.String>")
                || !inspectBase.getGenericReturnType().equals(inspectApi.getGenericReturnType()))
            throw new AssertionError("complete inherited fixed arguments changed");
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"), args[1].equals("synthetic"));
        for (Core self : new Core[]{new Core(), new Leaf()}) {
            int delta = self instanceof Leaf ? 3 : 1;
            for (int route = 0; route < 8; route++) {
                Object[] observed = new Object[2]; int[] callbacks = {0};
                self.observer = (value, token) -> {
                    if (self.seen != value || self.seenToken != token) throw new AssertionError("callback preceded original writes");
                    callbacks[0]++; observed[0] = value; observed[1] = token;
                };
                for (Object value : new Object[]{new Object(), "owned", new Object[]{self}, null}) {
                    int before = self.calls, events = callbacks[0]; Class token = value == null ? null : Object.class;
                    same(echo(route, self, token, value), value); same(self.seen, value); same(self.seenToken, token);
                    same(observed[0], value); same(observed[1], token);
                    state(self.calls, before + delta); state(callbacks[0], events + 1);
                    if (value instanceof Object[]) { ((Object[]) value)[0] = token; same(((Object[]) self.seen)[0], token); }
                    Carrier<Number, String> carrier = value == null ? null : new Carrier<>();
                    before = self.calls; events = callbacks[0];
                    same(inspect(route, self, carrier), carrier); same(self.seen, carrier); same(self.seenToken, null);
                    same(observed[0], carrier); same(observed[1], null);
                    state(self.calls, before + delta); state(callbacks[0], events + 1);
                    if (carrier != null) { carrier.second = "changed"; same(((Carrier) self.seen).second, "changed"); }
                }
                final int selected = route; Object input = new Object(); Carrier carrier = new Carrier();
                RuntimeException problem = new IllegalStateException("owned original provider failure");
                self.failure = problem;
                int before = self.calls, events = callbacks[0];
                failure(() -> echo(selected, self, Object.class, input), problem.getClass(), problem);
                same(self.seen, input); same(self.seenToken, Object.class);
                state(self.calls, before + delta); state(callbacks[0], events);
                before = self.calls;
                failure(() -> inspect(selected, self, carrier), problem.getClass(), problem);
                same(self.seen, carrier); same(self.seenToken, null);
                state(self.calls, before + delta); state(callbacks[0], events);
                self.failure = null;
                self.observer = (value, token) -> { callbacks[0]++; throw problem; };
                before = self.calls;
                failure(() -> echo(selected, self, null, input), problem.getClass(), problem);
                same(self.seen, input); same(self.seenToken, null);
                state(self.calls, before + delta); state(callbacks[0], ++events);
                before = self.calls;
                failure(() -> inspect(selected, self, null), problem.getClass(), problem);
                same(self.seen, null); same(self.seenToken, null);
                state(self.calls, before + delta); state(callbacks[0], ++events);
                before = self.calls;
                failure(() -> echo(selected, null, Object.class, input), NullPointerException.class, null);
                failure(() -> inspect(selected, null, carrier), NullPointerException.class, null);
                state(self.calls, before); state(callbacks[0], events);
                self.stored = input; same(self.value(), input); same(((Root) self).value(), input);
            }
        }
        System.out.println("identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-inherited-providers-and-full-method-bounds=retained,bridge-flags=reported");
    }
}
