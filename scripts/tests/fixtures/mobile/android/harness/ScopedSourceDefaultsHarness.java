import fixture.*;
import java.lang.reflect.*;

public final class ScopedSourceDefaultsHarness {
    private static int values, identities, states, failures;
    private interface Call { Object run() throws Throwable; }
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned scoped source default");
    }
    private static Object invoke(Method method, Object receiver) throws Throwable {
        try { return method.invoke(receiver); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Object call(int path, Receiver<String> receiver) throws Throwable {
        switch (path) {
            case 0: return receiver.getBox();
            case 1: return ((Source<String>) receiver).getBox();
            case 2: return ((Api<String>) receiver).getBox();
            case 3: return ((Api) receiver).getBox();
            case 4: return invoke(Receiver.class.getMethod("getBox"), receiver);
            default: throw new AssertionError(path);
        }
    }
    public static void main(String[] args) throws Throwable {
        Method source = Source.class.getDeclaredMethod("getBox");
        require(source.isDefault() && !source.isBridge() && !source.isSynthetic());
        require(source.getReturnType() == Box.class && source.getExceptionTypes().length == 0);
        require(source.getTypeParameters().length == 0);
        Type argument = ((ParameterizedType) source.getGenericReturnType()).getActualTypeArguments()[0];
        require(argument instanceof TypeVariable && ((TypeVariable<?>) argument).getGenericDeclaration() == Source.class);
        require(((TypeVariable<?>) argument).getName().equals("S"));
        require(Receiver.class.getMethod("getBox").getDeclaringClass() == Source.class);
        for (Method method : Receiver.class.getDeclaredMethods()) require(!method.getName().equals("getBox"));
        for (int path = 0; path < 5; path++) {
            for (int iteration = 0; iteration < 2; iteration++) {
                Receiver<String> receiver = new Receiver<String>();
                require(call(path, receiver) == receiver);
                values++; identities++;
                require(receiver.calls == 1); states++;
            }
            Receiver<String> untouched = new Receiver<String>();
            try { call(path, null); throw new AssertionError("missing null failure"); }
            catch (NullPointerException expected) { failures++; }
            require(untouched.calls == 0); states++;
        }
        require(values == 10 && identities == 10 && states == 15 && failures == 5);
        System.out.println("values=10,identities=10,states=15,failures=5");
        System.out.println("class-formal-default-return=retained,source-declaring-owner=retained,no-consumer-default-copy=retained");
    }
}
