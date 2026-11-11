import fixture.QueueView;
import java.lang.reflect.*;
import java.util.Iterator;
import java.util.Queue;

public final class SDKAbstractInterfaceRootsHarness {
    private static int values, identities, states, failures;
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned SDK abstract root contract");
    }
    private static Object invoke(Method method, Object receiver, Object value) throws Throwable {
        try { return method.invoke(receiver, value); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static boolean offer(int path, QueueView<String, Object> receiver, String value) throws Throwable {
        switch (path) {
            case 0: return receiver.offer(value);
            case 1: return ((Queue<String>) receiver).offer(value);
            case 2: return (Boolean) invoke(QueueView.class.getDeclaredMethod("offer", Object.class), receiver, value);
            default: throw new AssertionError(path);
        }
    }
    public static void main(String[] args) throws Throwable {
        require(QueueView.class.getTypeParameters().length == 2);
        Type parent = QueueView.class.getGenericSuperclass();
        require(parent instanceof ParameterizedType);
        require(((ParameterizedType) parent).getRawType() == java.util.PriorityQueue.class);
        require(((ParameterizedType) parent).getActualTypeArguments()[0] == QueueView.class.getTypeParameters()[0]);
        Method method = QueueView.class.getDeclaredMethod("offer", Object.class);
        require(!method.isBridge() && !method.isSynthetic());
        require(method.getGenericParameterTypes()[0] == QueueView.class.getTypeParameters()[0]);
        int originalOffers = 0;
        for (Method declared : QueueView.class.getDeclaredMethods()) {
            if (declared.getName().equals("offer")) originalOffers++;
        }
        require(originalOffers == 1);
        require(QueueView.class.getMethod("peek").getDeclaringClass() == java.util.PriorityQueue.class);
        for (int path = 0; path < 3; path++) {
            QueueView<String, Object> receiver = new QueueView<String, Object>();
            String c = new String("c"), a = new String("a"), b = new String("b");
            for (String value : new String[] {c, a, b}) { require(offer(path, receiver, value)); values++; }
            require(receiver.calls == 3 && receiver.size() == 3); states++;
            require(receiver.peek() == a); identities++;
            Iterator<String> iterator = receiver.iterator();
            boolean removed = false;
            while (iterator.hasNext()) {
                if (iterator.next() == b) { iterator.remove(); removed = true; }
            }
            require(removed && receiver.calls == 3 && receiver.size() == 2); states++;
            require(receiver.poll() == a); identities++;
            require(receiver.poll() == c); identities++;
            require(receiver.poll() == null); identities++;
            require(receiver.calls == 3 && receiver.size() == 0); states++;
            try { offer(path, receiver, null); throw new AssertionError("missing null input failure"); }
            catch (NullPointerException expected) { failures++; }
            require(receiver.calls == 4 && receiver.size() == 0); states++;
            try { offer(path, null, "a"); throw new AssertionError("missing null receiver failure"); }
            catch (NullPointerException expected) { failures++; }
            require(receiver.calls == 4 && receiver.size() == 0); states++;
        }
        require(values == 9 && identities == 12 && states == 15 && failures == 6);
        System.out.println("values=9,identities=12,states=15,failures=6");
        System.out.println("abstract-sdk-interface-root=retained,nearest-sdk-provider=retained,source-formal-override=retained");
    }
}
