import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class AbstractClassCovarianceHarness {
    private static int identities, states, failures;
    private interface Call { Root run(Core receiver, Object marker) throws Throwable; }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("abstract covariance alias changed");
        identities++;
    }
    private static Method entry(Class<?> owner, String name, Class<?> result, Class<?>... parameters) throws Exception {
        for (Method method : owner.getDeclaredMethods())
            if (method.getName().equals(name) && method.getReturnType() == result
                    && java.util.Arrays.equals(method.getParameterTypes(), parameters)) {
                method.setAccessible(true); return method;
            }
        throw new AssertionError("abstract covariance entry missing");
    }
    private static Object invoke(Method method, Object receiver, Object... parameters) throws Throwable {
        try { return method.invoke(receiver, parameters); }
        catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    private static final class Overriding extends Core {
        public Core get(Object marker) { calls += 3; return super.get(marker); }
        protected Core protectedGet() { calls += 3; return super.protectedGet(); }
    }
    private static void state(Core receiver, int before, Object marker, boolean changesMarker) {
        if (receiver.calls != before + (receiver instanceof Overriding ? 4 : 1)
                || (changesMarker && receiver.seen != marker))
            throw new AssertionError("abstract covariance effects changed");
        states++;
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        if (Middle.class.getSuperclass() != Root.class || Core.class.getSuperclass() != Middle.class
                || Middle.class.getTypeParameters().length != 0 || !Modifier.isAbstract(Middle.class.getModifiers()))
            throw new AssertionError("original abstract class role changed");
        int bridges = 0;
        for (Method method : Middle.class.getDeclaredMethods()) {
            if (!method.isBridge()) continue;
            if (!method.isSynthetic() || method.getReturnType() != Root.class || Modifier.isAbstract(method.getModifiers()))
                throw new AssertionError("compiler projection flags changed");
            bridges++;
        }
        if (bridges != (rebuilt ? 2 : 0)) throw new AssertionError("absent original slots were not disclosed");
        for (String name : new String[]{"get", "protectedGet"}) {
            Method declared = entry(Middle.class, name, Middle.class,
                name.equals("get") ? new Class<?>[]{Object.class} : new Class<?>[]{});
            if (!Modifier.isAbstract(declared.getModifiers()) || declared.isBridge() || declared.isSynthetic())
                throw new AssertionError("original abstract declaration changed");
        }
        Method formal = Middle.class.getDeclaredMethod("identity", Object.class);
        TypeVariable<?> variable = formal.getTypeParameters()[0];
        if (formal.getTypeParameters().length != 1 || !variable.getGenericDeclaration().equals(formal)
                || variable.getBounds()[0] != Object.class || !formal.getGenericReturnType().equals(variable)
                || !formal.getGenericParameterTypes()[0].equals(variable))
            throw new AssertionError("method-formal scope changed");
        java.util.List<Call> routes = new java.util.ArrayList<>();
        for (String name : new String[]{"get", "protectedGet"})
            for (Class<?> owner : new Class<?>[]{Root.class, Middle.class, Core.class}) {
                boolean argument = name.equals("get");
                Method method = entry(owner, name, owner,
                    argument ? new Class<?>[]{Object.class} : new Class<?>[]{});
                MethodHandle handle = MethodHandles.lookup().unreflect(method);
                routes.add((r, m) -> (Root) invoke(method, r, argument ? new Object[]{m} : new Object[]{}));
                routes.add((r, m) -> argument ? (Root) handle.invoke(r, m) : (Root) handle.invoke(r));
            }
        Core[] receivers = {new Core(), new Overriding()};
        Core[] results = {null, new Core(), new Overriding()};
        Object[] markers = {null, "owned", new Object()};
        for (Core receiver : receivers) for (Core result : results) for (Object marker : markers) {
            receiver.output = result;
            for (int route = 0; route < routes.size(); route++) {
                int before = receiver.calls;
                identity(routes.get(route).run(receiver, marker), result);
                if (result != null) result.tag = marker;
                identity(result == null ? null : result.tag, result == null ? null : marker);
                state(receiver, before, marker, route < 6);
            }
        }
        for (Core receiver : receivers) for (Object marker : markers) {
            int before = receiver.calls;
            identity(((Api) receiver).identity(marker), marker);
            if (receiver.calls != before + 1) throw new AssertionError("method-formal call changed");
            states++;
        }
        RuntimeException problem = new IllegalStateException("owned abstract covariance failure");
        for (Core receiver : receivers) {
            receiver.failure = problem;
            for (int route = 0; route < routes.size(); route++) {
                int before = receiver.calls;
                try { routes.get(route).run(receiver, problem); throw new AssertionError("failure missing"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                state(receiver, before, problem, route < 6);
            }
        }
        for (Call call : routes) {
            try { call.run(null, problem); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
        }
        System.out.println("values=216,identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("abstract-declarations-and-original-concrete-dispatch=retained,compiler-slots=reported");
    }
}
