import fixture.Contract;
import fixture.Holder;
import fixture.Mirror;
import fixture.Receiver;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

public final class DeferredAbstractInterfaceHarness {
    private static int identities;
    private static int failures;
    private static int values;

    private static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }

    private static void same(Object a, Object b) {
        identities++;
        check(a == b);
    }

    private static final class Child extends Receiver<Object, Object> {
        int calls;
        Child() { super(); }
        public void insert(int index, Object value) {
            calls++;
            super.insert(index ^ 0x5a5a5a5a, value);
        }
    }

    private static boolean reportedHelper(java.lang.reflect.Method method) {
        if (!method.getName().equals("__neverdThrow")) return false;
        check(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
        check(method.getParameterTypes().length == 1 && method.getParameterTypes()[0] == Throwable.class);
        check(method.getReturnType() == RuntimeException.class);
        return true;
    }

    public static void main(String[] args) throws Exception {
        Object first = new Object();
        Object second = new Object();
        Receiver<Object, Object> receiver = new Receiver<>();
        Child child = new Child();
        int[] indices = {Integer.MIN_VALUE, -1, 0, 1, Integer.MAX_VALUE};
        for (Receiver<Object, Object> target : new Receiver[] {receiver, child}) {
            Holder<Object, Object> holder = target;
            Contract<Object> contract = target;
            Mirror<Object> mirror = target;
            for (int index : indices) {
                for (Object item : new Object[] {first, second, null}) {
                    contract.insert(index, item);
                    values++;
                    check(target.lastIndex == (target == child ? index ^ 0x5a5a5a5a : index));
                    same(contract.current(), item);
                    same(mirror.current(), item);
                    same(holder.seed(), item);
                    same(contract.tail(), item);
                    same(mirror.tail(), item);
                    RuntimeException failure = new IllegalArgumentException("owned");
                    target.failure = failure;
                    try {
                        contract.insert(index, first);
                        throw new AssertionError();
                    } catch (RuntimeException actual) {
                        failures++;
                        same(actual, failure);
                    }
                    same(target.current(), item);
                    target.failure = null;
                }
            }
        }
        check(child.calls == 30);
        check(Modifier.isAbstract(Holder.class.getModifiers()));
        int holderMethods = 0;
        int helperMethods = 0;
        for (java.lang.reflect.Method method : Holder.class.getDeclaredMethods()) {
            if (reportedHelper(method)) helperMethods++;
            else holderMethods++;
        }
        check(holderMethods == 1);
        check(Holder.class.getDeclaredConstructors().length == 1);
        TypeVariable<?> formal = Holder.class.getTypeParameters()[0];
        check(formal.getName().equals("S"));
        same(Holder.class.getDeclaredField("state").getGenericType(), formal);
        same(Holder.class.getDeclaredMethod("seed").getGenericReturnType(), formal);
        Type[] interfaces = Holder.class.getGenericInterfaces();
        check(interfaces.length == 2);
        for (Type type : interfaces) {
            same(((ParameterizedType) type).getActualTypeArguments()[0], formal);
        }
        for (java.lang.reflect.Method method : Receiver.class.getDeclaredMethods()) {
            if (reportedHelper(method)) { helperMethods++; continue; }
            check(!method.isBridge() && !method.isSynthetic());
        }
        check(helperMethods == (args[0].equals("recovered") ? 2 : 0));
        TypeVariable<?> own = Receiver.class.getTypeParameters()[0];
        same(Receiver.class.getDeclaredMethod("insert", int.class, Object.class)
                .getGenericParameterTypes()[1], own);
        same(Receiver.class.getDeclaredMethod("current").getGenericReturnType(), own);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("inherited-abstract-slots=retained,method-inventory=unchanged,generic-reflection=unchanged");
    }
}
