import fixture.Reader;
import fixture.RootBase;
import fixture.StateView;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class PlainClassChainThrowsHarness {
    public static void main(String[] args) throws Throwable {
        PlainClassChainDefaultHarness.main(args);
        for (Class<?> owner : new Class<?>[] {StateView.class, RootBase.class, Reader.class}) {
            Class<?>[] failures = owner.getDeclaredMethod("rootState").getExceptionTypes();
            if (failures.length != 1 || failures[0] != IOException.class)
                throw new AssertionError("fixed ancestor exception declaration changed");
        }
        Reader<Object> reader = new Reader<>();
        IOException failure = new IOException("owned checked ancestor failure");
        reader.rootFailure = failure;
        Method method = Reader.class.getMethod("rootState");
        for (int path = 0; path < 3; path++) {
            try {
                if (path == 0) reader.rootState();
                else if (path == 1) ((StateView) reader).rootState();
                else method.invoke(reader);
                throw new AssertionError("original checked failure disappeared");
            } catch (IOException problem) {
                if (problem != failure) throw new AssertionError("checked failure identity changed");
            } catch (InvocationTargetException problem) {
                if (problem.getCause() != failure)
                    throw new AssertionError("reflected checked failure identity changed");
            }
            if (reader.rootCalls != 3) throw new AssertionError("checked failure reordered mutable state");
        }
        reader.rootFailure = null;
        if (reader.rootState() != 3) throw new AssertionError("normal ancestor result changed");
        System.out.println("fixed-ancestor-throws=retained,checked-failure-identities-and-reflection=matched");
    }
}
