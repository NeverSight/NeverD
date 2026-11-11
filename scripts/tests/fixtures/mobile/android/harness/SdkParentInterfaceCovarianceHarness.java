import fixture.*;
import java.lang.reflect.*;

public final class SdkParentInterfaceCovarianceHarness {
    private static int identities, states, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("SDK parent interface covariance identity changed");
        identities++;
    }
    private static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("SDK parent interface covariance effect changed");
        states++;
    }
    private static Method slot(Class<?> owner, Class<?> result) {
        for (Method method : owner.getDeclaredMethods())
            if (method.getName().equals("create") && method.getParameterCount() == 0
                    && method.getReturnType() == result) return method;
        throw new AssertionError("original interface result slot missing");
    }
    private static Object invoke(Method method, Object receiver) throws Exception {
        try { return method.invoke(receiver); }
        catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            if (cause instanceof RuntimeException) throw (RuntimeException) cause;
            throw error;
        }
    }
    private static final class Independent extends Reader {
        Independent(State value) { super(value); }
        public Leaf create() {
            covarianceCalls += 7;
            if (failure != null) throw failure;
            return covariance;
        }
    }
    private interface Call { Base run(Reader receiver) throws Exception; }
    public static void main(String[] args) throws Exception {
        PlainSdkReturnSlotsHarness.main(args);
        Method api = Api.class.getDeclaredMethod("create");
        Method typed = slot(Reader.class, Leaf.class);
        Method erased = slot(Reader.class, Base.class);
        if (api.getReturnType() != Base.class || typed.isBridge() || typed.isSynthetic()
                || !erased.isBridge() || !erased.isSynthetic()
                || !Modifier.isPublic(erased.getModifiers()) || erased.getExceptionTypes().length != 0
                || typed.getTypeParameters().length != 0 || api.getTypeParameters().length != 0
                || Reader.class.getTypeParameters().length != 0
                || Reader.class.getSuperclass() != SdkWidget.class
                || Leaf.class.getInterfaces().length != 1 || Leaf.class.getInterfaces()[0] != Base.class)
            throw new AssertionError("complete original plain covariant bridge metadata changed");
        Call[] calls = {r -> r.create(), r -> ((Api) r).create(),
                        r -> (Base) invoke(api, r), r -> (Base) invoke(typed, r), r -> (Base) invoke(erased, r)};
        Reader[] receivers = {new Reader(new State(null)), new Independent(new State(null))};
        Object[] markers = {null, new Object(), new Object[] {new Object()}};
        for (Reader receiver : receivers) {
            int step = receiver instanceof Independent ? 7 : 3;
            for (boolean absent : new boolean[] {false, true})
                for (Object marker : markers) {
                    receiver.covariance = absent ? null : new Leaf(marker);
                    for (Call call : calls) {
                        int before = receiver.covarianceCalls;
                        Base result = call.run(receiver);
                        identity(result, receiver.covariance);
                        if (result != null) {
                            identity(result.marker(), receiver.covariance.tag);
                            Object alias = new Object(); ((Leaf) result).tag = alias;
                            identity(receiver.covariance.tag, alias);
                        }
                        state(receiver.covarianceCalls, before + step);
                    }
                }
            RuntimeException problem = new IllegalStateException("owned SDK parent covariant failure");
            receiver.failure = problem;
            Leaf captured = receiver.covariance;
            for (Call call : calls) {
                int before = receiver.covarianceCalls;
                try { call.run(receiver); throw new AssertionError("covariant target failure lost"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                identity(receiver.covariance, captured);
                state(receiver.covarianceCalls, before + step);
            }
        }
        System.out.println("sdk-parent-interface-covariance-identities=" + identities + ",states=" + states
                + ",failures=" + failures + ",original-bridge=1");
        System.out.println("plain-sdk-ancestry-and-original-upcast=retained,independent-subclass-and-erased-dispatch=matched");
    }
}
