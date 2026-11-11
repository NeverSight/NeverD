import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class InheritedSelfCarrierHarness {
    private static int identities, states, failures;
    private interface Call { Carrier<?, ?> run(Provider<Object> receiver) throws Throwable; }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("self carrier identity changed");
        identities++;
    }
    private static Object invoke(Method method, Object receiver) throws Throwable {
        try { return method.invoke(receiver); }
        catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    private static final class Overriding extends Child<Object> {
        public Carrier<Object, Provider<Object>> result() { calls += 3; return super.result(); }
    }
    private static void state(Provider<Object> receiver, int before) {
        if (receiver.calls != before + (receiver instanceof Overriding ? 4 : 1))
            throw new AssertionError("self carrier call effects changed");
        states++;
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        Method method = Provider.class.getDeclaredMethod("result");
        Method required = Api.class.getDeclaredMethod("result");
        ParameterizedType apiResult = (ParameterizedType) required.getGenericReturnType();
        if (apiResult.getRawType() != Carrier.class
                || !apiResult.getActualTypeArguments()[0].equals(Api.class.getTypeParameters()[0])
                || !(apiResult.getActualTypeArguments()[1] instanceof ParameterizedType)
                || ((ParameterizedType) apiResult.getActualTypeArguments()[1]).getRawType() != Api.class)
            throw new AssertionError("original invariant interface result changed");
        if (rebuilt) {
            if (method.getGenericReturnType() != Carrier.class)
                throw new AssertionError("reported raw source result missing");
        } else {
            ParameterizedType result = (ParameterizedType) method.getGenericReturnType();
            if (result.getRawType() != Carrier.class
                    || !result.getActualTypeArguments()[0].equals(Provider.class.getTypeParameters()[0])
                    || ((ParameterizedType) result.getActualTypeArguments()[1]).getRawType() != Provider.class)
                throw new AssertionError("original complete provider result changed");
        }
        if (method.isBridge() || required.isBridge() || method.getExceptionTypes().length != 0
                || java.util.Arrays.stream(Provider.class.getDeclaredMethods()).filter(m -> !Modifier.isPrivate(m.getModifiers())).count() != 1
                || java.util.Arrays.stream(Child.class.getDeclaredMethods()).filter(m -> !Modifier.isPrivate(m.getModifiers())).count() != 0)
            throw new AssertionError("provider ownership or bridge inventory changed");
        MethodHandle handle = MethodHandles.lookup().unreflect(method);
        Call[] calls = {r -> r.result(), r -> ((Api<Object>) r).result(),
            r -> (Carrier<?, ?>) invoke(method, r), r -> (Carrier<?, ?>) handle.invoke(r),
            r -> SelfCarrierCaller.call(r)};
        Provider<Object>[] receivers = new Provider[]{new Provider<Object>(), new Child<Object>(), new Overriding()};
        Data<Object, Provider<Object>>[] results = new Data[]{null, new Data<Object, Provider<Object>>(null), new Data<Object, Provider<Object>>(null)};
        for (Provider<Object> receiver : receivers) for (Data<Object, Provider<Object>> result : results)
            for (Object marker : new Object[]{null, "owned", new Object()}) {
                receiver.value = result;
                if (result != null) result.value = marker;
                for (Call call : calls) {
                    int before = receiver.calls;
                    Carrier<?, ?> actual = call.run(receiver);
                    same(actual, result);
                    same(actual == null ? null : actual.tag(), result == null ? null : marker);
                    state(receiver, before);
                }
            }
        RuntimeException problem = new IllegalStateException("owned carrier failure");
        for (Provider<Object> receiver : receivers) {
            receiver.failure = problem;
            for (Call call : calls) {
                int before = receiver.calls;
                try { call.run(receiver); throw new AssertionError("failure missing"); }
                catch (RuntimeException actual) { same(actual, problem); failures++; }
                state(receiver, before);
            }
        }
        for (Call call : calls) {
            try { call.run(null); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
        }
        System.out.println("values=135,identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-inherited-provider-and-complete-ancestor-scopes=retained,raw-source-result=reported");
    }
}
