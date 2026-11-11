import fixture.Producer;
import fixture.Provider;
import fixture.Trace;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;

public final class PrivateCovariantHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("private covariant formatter changed");
    }
    static final class Marker {
        final Provider provider;
        final RuntimeException failure;
        final int mode;
        Marker(Provider provider, RuntimeException failure, int mode) {
            this.provider = provider; this.failure = failure; this.mode = mode;
        }
        public String toString() {
            Trace.hit(5);
            if (mode == 8) throw failure;
            if (mode == 10) provider.calls = 77;
            return "marker";
        }
    }
    static final class Maker implements Producer<Object> {
        final Provider provider;
        final Object value;
        final RuntimeException failure;
        final Error error;
        final int mode;
        int calls;
        Maker(Provider provider, Object value, RuntimeException failure, Error error, int mode) {
            this.provider = provider; this.value = value; this.failure = failure; this.error = error; this.mode = mode;
        }
        public Object get() {
            calls++; Trace.hit(3);
            require(Trace.receiver == provider);
            if (mode == 4) throw failure;
            if (mode == 5) throw error;
            if (mode == 7) provider.calls = 40;
            return value;
        }
    }
    static void contract() throws Exception {
        Method method = Provider.class.getDeclaredMethod("format", Producer.class, String.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        require(Provider.class.getTypeParameters().length == 0 && formals.length == 1 && formals[0].getName().equals("T"));
        require(formals[0].getGenericDeclaration().equals(method) && formals[0].getBounds().length == 1
            && formals[0].getBounds()[0] == Object.class && method.getGenericReturnType() == String.class);
        Type[] parameters = method.getGenericParameterTypes();
        require(parameters.length == 2 && parameters[1] == String.class && parameters[0] instanceof ParameterizedType);
        ParameterizedType producer = (ParameterizedType)parameters[0];
        require(producer.getRawType() == Producer.class && producer.getActualTypeArguments().length == 1);
        WildcardType slot = (WildcardType)producer.getActualTypeArguments()[0];
        require(slot.getLowerBounds().length == 0 && slot.getUpperBounds().length == 1 && slot.getUpperBounds()[0].equals(formals[0]));
        require(method.getModifiers() == (Modifier.PRIVATE | Modifier.FINAL) && method.getGenericExceptionTypes().length == 0);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        require(route >= 0 && route < 2 && mode >= 0 && mode < 12); contract();
        for (int attempt = 0; attempt < 2; attempt++) {
            Provider provider = new Provider(); provider.calls = 0; provider.seenContext = "sentinel";
            RuntimeException failure = new IllegalStateException("owned formatter failure");
            Error error = new AssertionError("owned formatter error");
            Object value = mode == 1 ? new String("value") : mode == 2 ? null
                : mode == 3 || mode == 8 || mode == 10 ? new Marker(provider, failure, mode)
                : mode == 11 ? new int[]{attempt} : new char[]{'x', 'y'};
            String context = mode == 9 ? null : new String("context:");
            String expected = String.valueOf(context) + (value == null ? "null" : value instanceof Marker ? "marker" : value.toString());
            Maker maker = new Maker(provider, value, failure, error, mode);
            Trace.trace = 0; Trace.receiver = null;
            Throwable caught = null; String result = null;
            try { result = PrivateCovariantCaller.call(route, provider, mode == 6 ? null : maker, context); }
            catch (Throwable thrown) { caught = thrown; }
            if (mode == 4 || mode == 8) require(caught == failure);
            else if (mode == 5) require(caught == error);
            else if (mode == 6) require(caught instanceof NullPointerException);
            else require(caught == null && expected.equals(result));
            boolean produced = mode != 4 && mode != 5 && mode != 6;
            require(maker.calls == (mode == 6 ? 0 : 1) && Trace.receiver == provider);
            require(provider.calls == (!produced ? 0 : mode == 7 ? 41 : mode == 10 ? 77 : 1));
            require(provider.seenContext == (produced ? context : "sentinel"));
            long trace = mode == 6 ? 12 : mode == 4 || mode == 5 ? 123 : mode == 8 ? 12345
                : value instanceof Marker ? 1234589 : 123489;
            require(Trace.trace == trace);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.trace);
        }
    }
}
