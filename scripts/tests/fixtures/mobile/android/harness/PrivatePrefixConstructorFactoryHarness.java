package probe;
import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class PrivatePrefixConstructorFactoryHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned private prefix factory mismatch"); }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable error) throws E { throw (E)error; }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean future = Boolean.parseBoolean(args[2]), enabled = Boolean.parseBoolean(args[3]);
        require(Child.class.getGenericSuperclass() instanceof ParameterizedType);
        ParameterizedType parent = (ParameterizedType)Child.class.getGenericSuperclass();
        require(parent.getActualTypeArguments()[0] == Value.class);
        Class<?> base = Child.class.getSuperclass();
        TypeVariable<?> formal = base.getTypeParameters()[0];
        require(formal.getBounds().length == 1 && formal.getBounds()[0] == Input.class);
        Constructor<?> inherited = base.getDeclaredConstructor(Input.class,Input.class);
        require(inherited.getModifiers() == Modifier.PROTECTED);
        require(inherited.getGenericParameterTypes()[0].equals(formal) && inherited.getGenericParameterTypes()[1] == Input.class);
        for (Method method : new Method[]{Child.class.getDeclaredMethod("first",boolean.class),Child.class.getDeclaredMethod("second")}) {
            require(method.getModifiers() == (Modifier.PRIVATE | Modifier.STATIC));
            require(method.getTypeParameters().length == 0 && method.getExceptionTypes().length == 0);
        }
        Constructor<Child> entry = Child.class.getConstructor(boolean.class);
        MethodHandle handle = MethodHandles.lookup().findConstructor(Child.class,MethodType.methodType(void.class,boolean.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Value left = mode == 1 && enabled ? null : future ? new PrivatePrefixConstructorFactoryCaller.Future() : new Value();
            final Value right = mode == 1 && !enabled ? null : future ? new PrivatePrefixConstructorFactoryCaller.Future() : new Value();
            final Value selected = enabled ? left : right;
            final Input initial = mode == 7 ? null : new Input() { };
            final Input replacement = new Input() { };
            final Throwable failure = mode == 2 || mode == 5 ? new java.io.IOException()
                : mode == 4 ? new AssertionError() : new IllegalStateException();
            final int bits = attempt == 0 ? 0x80000000 : 0x7fc12345;
            final int[] calls = {0,0,0,0};
            Trace.events = 0; Trace.inputFlag = !enabled; Trace.bits = 0; Trace.requestBits = bits;
            Trace.left = left; Trace.right = right; Trace.tail = initial;
            Trace.firstSeen = null; Trace.secondSeen = null; Trace.self = null; Trace.configured = null;
            Trace.configure = new Runnable() { public void run() {
                require(++calls[0] == 1 && Trace.configured == selected && Trace.bits == bits && Trace.inputFlag == enabled);
                if (mode == 2) PrivatePrefixConstructorFactoryHarness.<RuntimeException>raise(failure);
                if (mode == 6) { Trace.left = new Value(); Trace.right = new Value(); Trace.tail = replacement; }
            } };
            Trace.prepare = new Runnable() { public void run() {
                require(++calls[1] == 1 && Trace.inputFlag == enabled && Trace.self == null);
                require(Trace.tail == (mode == 6 ? replacement : initial));
                if (mode == 3) PrivatePrefixConstructorFactoryHarness.<RuntimeException>raise(failure);
            } };
            Trace.root = new Runnable() { public void run() {
                require(++calls[2] == 1 && Trace.self.getClass() == Child.class && !((Child)Trace.self).flag);
                require(Trace.firstSeen == null && Trace.secondSeen == null);
                if (mode == 4) PrivatePrefixConstructorFactoryHarness.<RuntimeException>raise(failure);
            } };
            Trace.base = new Runnable() { public void run() {
                require(++calls[3] == 1 && Trace.firstSeen == selected && Trace.secondSeen == null && !((Child)Trace.self).flag);
                if (mode == 5) PrivatePrefixConstructorFactoryHarness.<RuntimeException>raise(failure);
            } };
            Child made = null; Throwable caught = null;
            try {
                if (route == 0) made = new Child(enabled);
                else if (route == 1) made = PrivatePrefixConstructorFactoryCaller.call(enabled);
                else if (route == 2) try { made = entry.newInstance(enabled); }
                     catch (InvocationTargetException wrapped) { throw wrapped.getCause(); }
                else made = (Child)handle.invokeExact(enabled);
            } catch (Throwable error) { caught = error; }
            String first = mode == 1 ? "1" : future ? (mode == 2 ? "197" : "1978") : "17";
            String events = first + (mode == 2 ? "" : mode == 3 ? "2" : mode == 4 ? "23" : mode == 5 ? "234" : "23456");
            require(Trace.events == Long.parseLong(events));
            require(calls[0] == (mode == 1 ? 0 : 1) && calls[1] == (mode == 2 ? 0 : 1));
            require(calls[2] == (mode == 2 || mode == 3 ? 0 : 1) && calls[3] == (mode >= 2 && mode <= 4 ? 0 : 1));
            require(Trace.firstSeen == (mode >= 2 && mode <= 4 ? null : selected));
            boolean success = !(mode >= 2 && mode <= 5);
            require(Trace.secondSeen == (success ? mode == 6 ? replacement : initial : null));
            if (success) require(caught == null && made == Trace.self && made.flag == enabled);
            else require(caught == failure && made == null);
            if (Trace.self != null) require(((Child)Trace.self).flag == (success && enabled));
        }
        System.out.println("attempts=2,private-prefix-constructor-factory=matched");
    }
}
