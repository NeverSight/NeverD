import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InheritedFinalCovarianceHarness {
    public static class Child extends Core<Object, Object> {}
    private static int values, identities, states, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity"); identities++;
    }
    public static void main(String[] args) throws Throwable {
        Method child = Core.class.getDeclaredMethod("get");
        Method parent = Provider.class.getDeclaredMethod("get");
        Method api = Api.class.getDeclaredMethod("get");
        if (!child.isBridge() || !child.isSynthetic() || !Modifier.isPublic(child.getModifiers())
                || child.getReturnType() != Broad.class || parent.getReturnType() != Narrow.class
                || !Modifier.isFinal(parent.getModifiers())) throw new AssertionError("original bridge/final slots");
        MethodHandle childHandle = MethodHandles.publicLookup().unreflect(child);
        MethodHandle parentHandle = MethodHandles.publicLookup().unreflect(parent);
        Core[] receivers = {new Core<Object, Object>(), new Child()};
        Data[] data = {null, new Data<Object>(), new Data<Object>()};
        for (Core receiver : receivers)
            for (int route = 0; route < 8; route++)
                for (Data value : data)
                    for (int marker : new int[] {-5, 0, 41})
                        for (boolean throwing : new boolean[] {false, true}) {
                            Object alias = new Object();
                            if (value != null) { value.marker = marker; value.object = alias; }
                            RuntimeException failure = throwing ? new IllegalArgumentException("owned failure") : null;
                            receiver.value = value; receiver.failure = failure;
                            int before = receiver.calls;
                            try {
                                Broad result;
                                try {
                                    switch (route) {
                                        case 0: result = receiver.get(); break;
                                        case 1: result = ((Provider) receiver).get(); break;
                                        case 2: result = ((Api) receiver).get(); break;
                                        case 3: result = (Broad) child.invoke(receiver); break;
                                        case 4: result = (Broad) parent.invoke(receiver); break;
                                        case 5: result = (Broad) api.invoke(receiver); break;
                                        case 6: result = (Broad) childHandle.invoke(receiver); break;
                                        case 7: result = (Broad) parentHandle.invoke(receiver); break;
                                        default: throw new AssertionError("route");
                                    }
                                } catch (InvocationTargetException wrapped) { throw wrapped.getCause(); }
                                if (throwing) throw new AssertionError("missing failure");
                                identity(result, value);
                                if (result != null) {
                                    if (result.mark() != marker) throw new AssertionError("mutable value"); values++;
                                    identity(result.alias(), alias);
                                }
                            } catch (RuntimeException caught) {
                                if (!throwing) throw caught;
                                identity(caught, failure); failures++;
                            }
                            if (receiver.calls != before + 1) throw new AssertionError("call effects"); states++;
                            identity(receiver.last, value);
                        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-inherited-final-provider-and-covariant-bridge=retained,complete-metadata=unchanged");
    }
}
