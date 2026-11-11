package probe;
import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
public final class InstanceArrayCarrierVoidHarness {
    public static final class FuturePayload extends Payload { public FuturePayload(int value) { super(value); } }
    public static final class FutureProvider extends Provider {
        public <T> void fill(T[] values, Contract.Creator<T> creator) {
            ++Trace.overrides; super.fill(values, creator);
        }
    }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned instance array carrier void mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static void helper(Method method) {
        require(method.getModifiers() == (Modifier.PRIVATE | Modifier.STATIC));
        require(method.getReturnType() == RuntimeException.class && Arrays.equals(method.getParameterTypes(), new Class<?>[]{Throwable.class}));
        require(method.getTypeParameters().length == 1 && Arrays.equals(method.getExceptionTypes(), new Class<?>[]{Throwable.class}));
        TypeVariable<?> variable = method.getTypeParameters()[0];
        require(Arrays.equals(variable.getBounds(), new Type[]{Throwable.class}));
        require(Arrays.equals(method.getGenericExceptionTypes(), new Type[]{variable}));
        require(method.getDeclaredAnnotations().length == 0);
    }
    static void identity() throws Exception {
        require(Provider.class.getModifiers() == Modifier.PUBLIC && Provider.class.getTypeParameters().length == 0);
        int count = 0;
        for (Method method : Provider.class.getDeclaredMethods()) {
            if (method.getName().equals("__neverdThrow")) { helper(method); continue; }
            ++count;
            require(method.getName().equals("fill") && method.getModifiers() == Modifier.PUBLIC && !method.isBridge() && !method.isSynthetic());
            require(method.getReturnType() == void.class && method.getExceptionTypes().length == 0 && method.getDeclaredAnnotations().length == 0);
            require(Arrays.equals(method.getParameterTypes(), new Class<?>[]{Object[].class, Contract.Creator.class}));
            TypeVariable<?>[] variables = method.getTypeParameters();
            require(variables.length == 1 && Arrays.equals(variables[0].getBounds(), new Type[]{Object.class}));
            Type[] parameters = method.getGenericParameterTypes();
            require(parameters[0] instanceof GenericArrayType && ((GenericArrayType) parameters[0]).getGenericComponentType().equals(variables[0]));
            require(parameters[1] instanceof ParameterizedType);
            ParameterizedType carrier = (ParameterizedType) parameters[1];
            require(carrier.getRawType() == Contract.Creator.class && Arrays.equals(carrier.getActualTypeArguments(), new Type[]{variables[0]}));
        }
        require(count == 1 && Provider.class.getDeclaredConstructors().length == 1 && Provider.class.getDeclaredFields().length == 0);
        require(Contract.Creator.class.getModifiers() == (Modifier.PUBLIC | Modifier.STATIC | Modifier.INTERFACE | Modifier.ABSTRACT));
        require(Contract.Creator.class.getEnclosingClass() == Contract.class && Contract.Creator.class.isMemberClass());
        TypeVariable<?>[] formals = Contract.Creator.class.getTypeParameters();
        require(formals.length == 1 && Arrays.equals(formals[0].getBounds(), new Type[]{Object.class}));
        Method next = Contract.Creator.class.getDeclaredMethod("next");
        require(next.getGenericReturnType().equals(formals[0]) && next.getReturnType() == Object.class && next.getParameterCount() == 0);
        require(Contract.Creator.class.getDeclaredMethods().length == 1);
        int entries = 0;
        for (Method method : Owner.class.getDeclaredMethods()) {
            if (method.getName().equals("__neverdThrow")) { helper(method); continue; }
            require(method.getName().equals("create") && method.getModifiers() == (Modifier.PUBLIC | Modifier.STATIC));
            ++entries;
        }
        require(entries == 1 && Owner.class.getDeclaredConstructors().length == 1);
        Constructor<?> constructor = Owner.class.getDeclaredConstructors()[0];
        require(constructor.getModifiers() == Modifier.PUBLIC && constructor.getExceptionTypes().length == 0);
        require(Arrays.equals(constructor.getParameterTypes(), new Class<?>[]{Provider.class, Payload[].class, Contract.Creator.class}));
        Type parameter = constructor.getGenericParameterTypes()[2];
        require(parameter instanceof ParameterizedType && Arrays.equals(((ParameterizedType) parameter).getActualTypeArguments(), new Type[]{Payload.class}));
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final boolean future = Boolean.parseBoolean(args[2]);
        identity();
        final Constructor<Owner> constructor = Owner.class.getConstructor(Provider.class, Payload[].class, Contract.Creator.class);
        final MethodHandle handle = MethodHandles.lookup().findConstructor(Owner.class,
                MethodType.methodType(void.class, Provider.class, Payload[].class, Contract.Creator.class));
        final Provider first = future ? new FutureProvider() : new Provider(), other = new Provider();
        for (int attempt = 0; attempt < 2; ++attempt) {
            final boolean empty = mode == 2 || mode == 20, noArray = mode == 3 || mode == 19;
            final boolean noCreator = mode == 4 || mode == 19 || mode == 20, noProvider = mode == 5 || mode == 18;
            final boolean narrow = mode == 1 || mode == 12;
            final Payload marker = narrow ? new FuturePayload(-1) : new Payload(-1);
            final Payload[] values = noArray ? null : narrow ? new FuturePayload[3] : new Payload[empty ? 0 : 3];
            final Payload[] replacement = new Payload[3];
            if (values != null) Arrays.fill(values, marker);
            final Payload[] results = {new FuturePayload(11 + attempt), new FuturePayload(22 + attempt), new FuturePayload(33 + attempt)};
            final Throwable selected = mode == 7 ? new IllegalStateException() : mode == 8 ? new AssertionError() : new java.io.IOException();
            final int[] calls = {0, 0, 0, 0};
            final boolean supplierFailure = mode >= 14 && mode <= 16 || mode == 18;
            final boolean body = !supplierFailure && !noProvider;
            final boolean callbackFailure = mode >= 6 && mode <= 9 || mode == 12 || mode == 13;
            final int written = !body || noArray || noCreator || empty || mode == 13 ? 0 : callbackFailure ? 1 : 3;
            final int called = !body || noArray || noCreator || empty ? 0 : mode == 13 ? 1 : callbackFailure ? 2 : 3;
            final boolean complete = body && !noArray && (empty || !noCreator && !callbackFailure);
            Trace.events = 0; Trace.stores = 0; Trace.overrides = 0; Trace.owner = null;
            Trace.receiver = null; Trace.array = null; Trace.usedCreator = null;
            Trace.provider = noProvider ? null : first; Trace.values = values;
            final Contract.Creator raw = new Contract.Creator() { public Object next() {
                final int index = calls[3]++;
                require(index < 3 && Trace.stores == index && Trace.receiver == first && Trace.array == values);
                require(Trace.events == Long.parseLong("78912" + "34".repeat(index) + "3"));
                require(Trace.owner != null && Trace.owner.saved == null);
                if (mode == 13 || index == 1 && mode >= 6 && mode <= 8)
                    InstanceArrayCarrierVoidHarness.<RuntimeException>raise(selected);
                if (index == 1 && mode == 9) return new Object();
                if (index == 1 && mode == 12) return new Payload(44);
                if (mode == 11 && index == 1) { values[0] = marker; values[2] = marker; }
                if (mode == 22) { Trace.values = replacement; Trace.provider = other; }
                if (mode == 21 || mode == 10 && index == 1) return null;
                return results[mode == 23 ? 0 : index];
            }};
            final Contract.Creator<Payload> creator = noCreator ? null : raw;
            Trace.creator = creator;
            Trace.receiverAction = new Runnable() { public void run() {
                require(++calls[0] == 1 && Trace.events == 7);
                if (mode == 14) InstanceArrayCarrierVoidHarness.<RuntimeException>raise(selected);
            }};
            Trace.arrayAction = new Runnable() { public void run() {
                require(++calls[1] == 1 && Trace.events == 78);
                if (mode == 15 || mode == 18) InstanceArrayCarrierVoidHarness.<RuntimeException>raise(selected);
                if (mode == 17) Trace.provider = other;
            }};
            Trace.creatorAction = new Runnable() { public void run() {
                require(++calls[2] == 1 && Trace.events == 789);
                if (mode == 16) InstanceArrayCarrierVoidHarness.<RuntimeException>raise(selected);
                if (mode == 17) Trace.values = replacement;
            }};
            Throwable failure = null; Owner result = null;
            try { result = InstanceArrayCarrierVoidCaller.call(route, constructor, handle); }
            catch (Throwable caught) { failure = caught; }
            String trace = mode == 14 ? "7" : mode == 15 || mode == 18 ? "78" : mode == 16 ? "789" : "7891";
            if (body) {
                trace += "2";
                if (!noArray && !empty) trace += "34".repeat(written) + (complete ? "" : "3");
                if (complete) trace += "56";
            }
            require(Trace.events == Long.parseLong(trace));
            require(calls[0] == 1 && calls[1] == (mode == 14 ? 0 : 1) && calls[2] == (mode == 14 || mode == 15 || mode == 18 ? 0 : 1));
            require(calls[3] == called && Trace.stores == written && Trace.overrides == (body && future ? 1 : 0));
            require(Trace.receiver == (body ? first : null) && Trace.array == (body ? values : null) && Trace.usedCreator == (body ? creator : null));
            require((Trace.owner != null) == !supplierFailure);
            if (Trace.owner != null) require(Trace.owner.saved == (complete ? values : null));
            if (values != null) for (int index = 0; index < values.length; ++index) {
                Payload expected = index < written ? mode == 21 || mode == 10 && index == 1 ? null : results[mode == 23 ? 0 : index] : marker;
                if (mode == 11 && index == 0) expected = marker;
                require(values[index] == expected);
            }
            if (complete) require(failure == null && result == Trace.owner);
            else {
                require(result == null);
                if (supplierFailure || body && !noArray && !noCreator && (mode >= 6 && mode <= 8 || mode == 13)) require(failure == selected);
                else if (mode == 9 || mode == 12) require(failure instanceof ArrayStoreException);
                else require(failure instanceof NullPointerException);
            }
            identity();
        }
        System.out.println("attempts=2,instance-array-carrier-void=matched");
    }
}
