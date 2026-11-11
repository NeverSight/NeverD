import fixture.*;
import java.lang.reflect.*;

public final class PlainSdkReturnSlotsHarness {
    private static int values, identities, states, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("plain return-slot identity changed");
        identities++;
    }
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("SDK scalar slot changed");
        values++;
    }
    private static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("plain return-slot state changed");
        states++;
    }
    private static Object invoke(Method method, Object receiver, Object... arguments) throws Exception {
        try { return method.invoke(receiver, arguments); }
        catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            if (cause instanceof RuntimeException) throw (RuntimeException) cause;
            throw error;
        }
    }
    private interface SourceCall { Object run(Reader receiver) throws Exception; }
    private interface SdkCall { int run(SdkRoot receiver) throws Exception; }
    private static final class IndependentSdk extends SdkWidget {
        public int getState() { sdkCalls += 5; return super.getState(); }
    }
    public static void main(String[] args) throws Exception {
        String name = args[0];
        Method api = Api.class.getDeclaredMethod(name);
        Method reader = Reader.class.getDeclaredMethod(name);
        Method sdk = SdkRoot.class.getDeclaredMethod("getState");
        if (api.getReturnType() != State.class || reader.getReturnType() != State.class
                || sdk.getReturnType() != int.class || sdk.getDeclaringClass() != SdkRoot.class)
            throw new AssertionError("separate original runtime slots changed");
        boolean covariant = args.length == 2 && args[1].equals("interface-covariance");
        int originalBridges = 0;
        for (Class<?> owner : new Class<?>[] {Api.class, Reader.class, Overriding.class}) {
            if (owner.getTypeParameters().length != 0)
                throw new AssertionError("plain return-slot class borrowed a scope");
            for (Method method : owner.getDeclaredMethods())
                if (method.isBridge()) {
                    if (!covariant || owner != Reader.class || !method.getName().equals("create")
                            || method.getParameterCount() != 0 || !method.isSynthetic()
                            || !Modifier.isPublic(method.getModifiers())
                            || !method.getReturnType().getName().equals("fixture.Base"))
                        throw new AssertionError("plain return-slot bridge invented");
                    originalBridges++;
                }
        }
        if (originalBridges != (covariant ? 1 : 0))
            throw new AssertionError("original covariant bridge inventory changed");
        Method generic = Reader.class.getDeclaredMethod("identity", Object.class);
        TypeVariable<Method> formal = generic.getTypeParameters()[0];
        if (!formal.getName().equals("N") || generic.getGenericReturnType() != formal
                || generic.getGenericParameterTypes()[0] != formal)
            throw new AssertionError("unrelated method-owned generic changed");
        if (!name.equals("getState") && Reader.class.getMethod("getState").getReturnType() != int.class)
            throw new AssertionError("source alias hid unchanged SDK scalar slot");
        value(Api.__neverdReturnSlot(), 123);
        identity(new Unrelated().getState(), null);
        SourceCall[] source = {
            r -> Caller.direct(r), r -> Caller.contract(r),
            r -> r instanceof ForwardReader ? Caller.inherited((ForwardReader) r) : Caller.direct(r),
            r -> invoke(api, r), r -> invoke(reader, r), r -> invoke(r.getClass().getMethod(name), r)
        };
        SdkCall[] scalars = {r -> r.getState(), r -> Caller.scalar(r), r -> (Integer) invoke(sdk, r)};
        Reader[] receivers = {new Reader(new State(null)), new ForwardReader(new State(null)),
                              new Overriding(new State(null))};
        Object[] markers = {null, new Object(), new Object[] {new Object()}};
        int[] numbers = {0, -7, Integer.MAX_VALUE};
        for (Reader receiver : receivers) {
            int step = receiver instanceof Overriding ? 7 : 3;
            for (int i = 0; i < markers.length; ++i) {
                receiver.value.tag = markers[i]; receiver.scalar = numbers[i];
                for (SourceCall call : source) {
                    int before = receiver.sourceCalls;
                    State result = (State) call.run(receiver);
                    identity(result, receiver.value); identity(result.tag, receiver.value.tag);
                    Object alias = new Object(); result.tag = alias; identity(receiver.value.tag, alias);
                    state(receiver.sourceCalls, before + step);
                }
                for (SdkCall call : scalars) {
                    int before = receiver.sdkCalls;
                    value(call.run(receiver), numbers[i]); state(receiver.sdkCalls, before + 1);
                }
                int before = receiver.identityCalls;
                identity(((Api) receiver).identity(markers[i]), markers[i]); state(receiver.identityCalls, before + 1);
            }
            RuntimeException problem = new IllegalStateException("owned distinct-slot failure");
            receiver.failure = problem;
            for (SourceCall call : source) {
                int before = receiver.sourceCalls;
                try { call.run(receiver); throw new AssertionError("source slot failure lost"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                state(receiver.sourceCalls, before + step);
            }
            for (SdkCall call : scalars) {
                int before = receiver.sdkCalls;
                try { call.run(receiver); throw new AssertionError("SDK slot failure lost"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                state(receiver.sdkCalls, before + 1);
            }
            int before = receiver.identityCalls;
            try { ((Api) receiver).identity(problem); throw new AssertionError("generic failure lost"); }
            catch (RuntimeException actual) { identity(actual, problem); failures++; }
            state(receiver.identityCalls, before + 1);
        }
        IndependentSdk independent = new IndependentSdk();
        for (SdkCall call : scalars) {
            int before = independent.sdkCalls;
            value(call.run(independent), 7); state(independent.sdkCalls, before + 6);
        }
        RuntimeException problem = new IllegalStateException("owned independent SDK failure");
        independent.failure = problem;
        for (SdkCall call : scalars) {
            int before = independent.sdkCalls;
            try { call.run(independent); throw new AssertionError("independent SDK failure lost"); }
            catch (RuntimeException actual) { identity(actual, problem); failures++; }
            state(independent.sdkCalls, before + 6);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("distinct-input-reference-and-unchanged-sdk-scalar-slots=matched,source-name-and-abi-differences=reported");
    }
}
