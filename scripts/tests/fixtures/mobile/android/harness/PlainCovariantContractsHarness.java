import fixture.*;
import java.lang.reflect.*;

public final class PlainCovariantContractsHarness {
    private static int identities, values, failures;
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity changed");
        identities++;
    }
    private static void count(int actual, int expected) {
        if (actual != expected) throw new AssertionError("effects changed");
        values++;
    }
    private static void check(boolean value) {
        if (!value) throw new AssertionError("declaration changed");
    }
    private static boolean helper(Method method) {
        if (!method.getName().equals("__neverdThrow")) return false;
        check(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
        check(method.getReturnType() == RuntimeException.class && method.getParameterCount() == 1);
        check(method.getParameterTypes()[0] == Throwable.class);
        return true;
    }
    private static void reflection() throws Exception {
        check(Receiver.class.getGenericSuperclass().getTypeName().equals("fixture.Producer<java.lang.String>"));
        check(Receiver.class.getInterfaces()[0] == PlainApi.class);
        check(PlainApi.class.getGenericInterfaces()[0].getTypeName().equals("fixture.GenericApi<java.lang.String>"));
        check(BaseValue.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Result<java.lang.String>"));
        check(SpecificValue.class.getGenericSuperclass() == BaseValue.class);
        check(SpecificValue.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Marker<java.lang.Integer>"));
        ParameterizedType parentResult = (ParameterizedType) Producer.class.getMethod("create").getGenericReturnType();
        check(parentResult.getRawType() == Result.class);
        check(parentResult.getActualTypeArguments()[0].equals(Producer.class.getTypeParameters()[0]));
        int receiverBridges = 0, plainBridges = 0;
        for (Method method : Receiver.class.getDeclaredMethods()) {
            if (helper(method)) continue;
            check(method.getName().equals("create") && method.getParameterCount() == 0);
            check(method.getExceptionTypes().length == 0);
            if (method.getReturnType() == SpecificValue.class) {
                check(!method.isBridge() && !method.isSynthetic());
                check(method.getGenericReturnType() == SpecificValue.class);
            } else {
                check(method.getReturnType() == BaseValue.class || method.getReturnType() == Result.class);
                check(method.isBridge() && method.isSynthetic());
                receiverBridges++;
            }
        }
        for (Method method : PlainApi.class.getDeclaredMethods()) {
            if (helper(method)) continue;
            check(method.getName().equals("create") && method.getParameterCount() == 0);
            if (method.getReturnType() == BaseValue.class) {
                check(Modifier.isAbstract(method.getModifiers()) && !method.isBridge());
                check(method.getGenericReturnType() == BaseValue.class);
            } else {
                check(method.getReturnType() == Result.class && method.isBridge() && method.isSynthetic());
                check(method.isDefault());
                plainBridges++;
            }
        }
        check(receiverBridges == 2 && plainBridges == 1);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Exception {
        Receiver receiver = new Receiver();
        Producer<String> parent = receiver;
        PlainApi plain = receiver;
        GenericApi<String> generic = receiver;
        GenericApi raw = receiver;
        SpecificValue value = new SpecificValue();
        receiver.value = value;
        same(receiver.create(), value); same(parent.create(), value);
        same(plain.create(), value); same(generic.create(), value); same(raw.create(), value);
        count(receiver.calls, 5);
        Object payload = new Object();
        value.payload = payload; same(value.payload, payload);
        receiver.value = null;
        same(receiver.create(), null); same(parent.create(), null);
        same(plain.create(), null); same(generic.create(), null); same(raw.create(), null);
        count(receiver.calls, 10);
        RuntimeException sentinel = new IllegalStateException("owned target failure");
        receiver.failure = sentinel;
        receiver.parentFailure = new IllegalArgumentException("wrong ancestor dispatch");
        Runnable[] calls = {() -> receiver.create(), () -> parent.create(), () -> plain.create(),
                            () -> generic.create(), () -> raw.create()};
        for (int i = 0; i < calls.length; ++i) {
            try { calls[i].run(); throw new AssertionError("failure lost"); }
            catch (RuntimeException error) { same(error, sentinel); failures++; }
            count(receiver.calls, 11 + i);
        }
        count(receiver.parentCalls, 0);
        GenericApi<String> missing = null;
        try { missing.create(); throw new AssertionError("null receiver accepted"); }
        catch (NullPointerException expected) { failures++; }
        reflection();
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("plain-contracts=retained,full-result-ancestry=matched,original-bridges=retained");
    }
}
